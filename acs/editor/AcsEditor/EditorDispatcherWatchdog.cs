// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

internal readonly record struct EditorDispatcherWatchdogTransition(
    long Sequence,
    DateTimeOffset ObservedUtc,
    bool Recovered,
    double DurationMilliseconds,
    string Phase,
    int StallCount);

internal readonly record struct EditorDispatcherWatchdogSnapshot(
    long HeartbeatCount,
    double HeartbeatAgeMilliseconds,
    double LastDispatcherGapMilliseconds,
    double MaximumDispatcherGapMilliseconds,
    int StallCount,
    bool StallActive,
    double ActiveStallMilliseconds,
    double LongestStallMilliseconds,
    string Phase);

/// <summary>
/// Watches the WPF Dispatcher from a ThreadPool timer. A DispatcherTimer alone
/// cannot record evidence while the UI thread is blocked, so the watchdog
/// keeps a bounded in-memory critical section and publishes transitions after
/// releasing it; diagnostic I/O remains the owner's worker-thread concern.
/// </summary>
internal sealed class EditorDispatcherWatchdog : IDisposable
{
    internal const double DefaultStallThresholdMilliseconds = 2000.0;
    internal const double DefaultPollIntervalMilliseconds = 250.0;

    private readonly object _stateLock = new();
    private readonly Queue<EditorDispatcherWatchdogTransition>
        _pendingTransitions = new();
    private readonly ManualResetEventSlim _transitionIdle =
        new(initialState: true);
    private readonly Action<EditorDispatcherWatchdogTransition> _transitionSink;
    private readonly Func<long> _timestampProvider;
    private readonly double _timestampFrequency;
    private readonly long _stallThresholdTicks;
    private Timer? _timer;
    private long _lastHeartbeatTimestamp;
    private long _heartbeatCount;
    private long _lastDispatcherGapTicks;
    private long _maximumDispatcherGapTicks;
    private long _longestStallTicks;
    private int _stallCount;
    private int _stallActive;
    private int _disposed;
    private bool _transitionPumpRunning;
    private long _transitionSequence;
    private string _phase;
    private string _activeStallPhase = "none";

    internal EditorDispatcherWatchdog(
        Action<EditorDispatcherWatchdogTransition> transitionSink,
        TimeSpan? stallThreshold = null,
        TimeSpan? pollInterval = null,
        Func<long>? timestampProvider = null,
        double? timestampFrequency = null,
        bool startAutomatically = true)
    {
        ArgumentNullException.ThrowIfNull(transitionSink);
        TimeSpan threshold = stallThreshold ??
            TimeSpan.FromMilliseconds(DefaultStallThresholdMilliseconds);
        TimeSpan interval = pollInterval ??
            TimeSpan.FromMilliseconds(DefaultPollIntervalMilliseconds);
        if (!double.IsFinite(threshold.TotalMilliseconds) ||
            threshold <= TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(nameof(stallThreshold));
        }
        if (!double.IsFinite(interval.TotalMilliseconds) ||
            interval <= TimeSpan.Zero ||
            interval > threshold)
        {
            throw new ArgumentOutOfRangeException(nameof(pollInterval));
        }

        _transitionSink = transitionSink;
        _timestampProvider = timestampProvider ?? Stopwatch.GetTimestamp;
        _timestampFrequency = timestampFrequency ?? Stopwatch.Frequency;
        if (!double.IsFinite(_timestampFrequency) || _timestampFrequency <= 0)
            throw new ArgumentOutOfRangeException(nameof(timestampFrequency));

        _stallThresholdTicks = DurationToTimestampTicks(
            threshold,
            _timestampFrequency);
        _phase = "constructing editor";
        _lastHeartbeatTimestamp = _timestampProvider();
        _heartbeatCount = 1;

        if (startAutomatically)
        {
            _timer = new Timer(
                static state => ((EditorDispatcherWatchdog)state!).Poll(),
                this,
                interval,
                interval);
        }
    }

    internal void Beat(string phase)
    {
        if (Volatile.Read(ref _disposed) != 0)
            return;

        lock (_stateLock)
        {
            if (_disposed != 0)
                return;

            long now = _timestampProvider();
            string stalledPhase = _stallActive != 0
                ? _activeStallPhase
                : _phase;
            _phase = string.IsNullOrWhiteSpace(phase) ? "unknown" : phase;
            long gap = ElapsedTicks(_lastHeartbeatTimestamp, now);
            _lastHeartbeatTimestamp = now;
            _heartbeatCount++;
            _lastDispatcherGapTicks = gap;
            _maximumDispatcherGapTicks = Math.Max(
                _maximumDispatcherGapTicks,
                gap);

            if (_stallActive != 0)
            {
                _stallActive = 0;
                _activeStallPhase = "none";
                _longestStallTicks = Math.Max(_longestStallTicks, gap);
                EnqueueTransitionLocked(
                    recovered: true,
                    gap,
                    stalledPhase);
                return;
            }

            // The ThreadPool poll may itself be delayed by GC or CPU
            // starvation. A heartbeat that observes the full threshold still
            // records a completed stall instead of silently losing evidence.
            if (gap < _stallThresholdTicks)
                return;

            _stallCount++;
            _longestStallTicks = Math.Max(_longestStallTicks, gap);
            EnqueueTransitionLocked(
                recovered: false,
                gap,
                stalledPhase);
            EnqueueTransitionLocked(
                recovered: true,
                gap,
                stalledPhase);
        }
    }

    internal void SetPhase(string phase)
    {
        if (Volatile.Read(ref _disposed) != 0)
            return;
        lock (_stateLock)
        {
            if (_disposed == 0)
            {
                _phase = string.IsNullOrWhiteSpace(phase)
                    ? "unknown"
                    : phase;
            }
        }
    }

    internal EditorDispatcherWatchdogSnapshot Snapshot()
    {
        lock (_stateLock)
        {
            long heartbeatAge = ElapsedTicks(
                _lastHeartbeatTimestamp,
                _timestampProvider());
            bool stallActive = _stallActive != 0;
            long longest = stallActive
                ? Math.Max(_longestStallTicks, heartbeatAge)
                : _longestStallTicks;
            return new EditorDispatcherWatchdogSnapshot(
                HeartbeatCount: _heartbeatCount,
                HeartbeatAgeMilliseconds: ToMilliseconds(heartbeatAge),
                LastDispatcherGapMilliseconds: ToMilliseconds(
                    _lastDispatcherGapTicks),
                MaximumDispatcherGapMilliseconds: ToMilliseconds(
                    _maximumDispatcherGapTicks),
                StallCount: _stallCount,
                StallActive: stallActive,
                ActiveStallMilliseconds: stallActive
                    ? ToMilliseconds(heartbeatAge)
                    : 0,
                LongestStallMilliseconds: ToMilliseconds(longest),
                Phase: stallActive ? _activeStallPhase : _phase);
        }
    }

    internal void ResetPeaks()
    {
        lock (_stateLock)
        {
            _lastHeartbeatTimestamp = _timestampProvider();
            _heartbeatCount = 0;
            _lastDispatcherGapTicks = 0;
            _maximumDispatcherGapTicks = 0;
            _longestStallTicks = 0;
            _stallCount = 0;
            _stallActive = 0;
            _activeStallPhase = "none";
        }
    }

    internal void PollForSelfTest() => Poll();
    internal long StallThresholdTicksForSelfTest => _stallThresholdTicks;

    private void Poll()
    {
        if (Volatile.Read(ref _disposed) != 0)
            return;

        lock (_stateLock)
        {
            if (_disposed != 0)
                return;

            long age = ElapsedTicks(
                _lastHeartbeatTimestamp,
                _timestampProvider());
            if (age < _stallThresholdTicks || _stallActive != 0)
                return;

            _stallActive = 1;
            _stallCount++;
            _activeStallPhase = _phase;
            _longestStallTicks = Math.Max(_longestStallTicks, age);
            EnqueueTransitionLocked(
                recovered: false,
                age,
                _activeStallPhase);
        }
    }

    private void EnqueueTransitionLocked(
        bool recovered,
        long durationTicks,
        string phase)
    {
        _pendingTransitions.Enqueue(
            new EditorDispatcherWatchdogTransition(
                Sequence: ++_transitionSequence,
                ObservedUtc: DateTimeOffset.UtcNow,
                Recovered: recovered,
                DurationMilliseconds: ToMilliseconds(durationTicks),
                Phase: phase,
                StallCount: _stallCount));
        _transitionIdle.Reset();
        if (_transitionPumpRunning)
            return;
        _transitionPumpRunning = true;
        _ = Task.Run(DrainTransitions);
    }

    private void DrainTransitions()
    {
        while (true)
        {
            EditorDispatcherWatchdogTransition transition;
            lock (_stateLock)
            {
                if (_pendingTransitions.Count == 0)
                {
                    _transitionPumpRunning = false;
                    _transitionIdle.Set();
                    return;
                }
                transition = _pendingTransitions.Dequeue();
            }
            try
            {
                _transitionSink(transition);
            }
            catch
            {
                // Diagnostics must never become another editor failure path.
            }
        }
    }

    internal bool WaitForTransitionsForSelfTest(TimeSpan timeout) =>
        _transitionIdle.Wait(timeout);

    private double ToMilliseconds(long ticks) =>
        Math.Max(0, ticks) * 1000.0 / _timestampFrequency;

    private static long DurationToTimestampTicks(
        TimeSpan duration,
        double timestampFrequency)
    {
        double ticks = duration.TotalSeconds * timestampFrequency;
        if (!double.IsFinite(ticks) || ticks >= long.MaxValue)
            return long.MaxValue;
        return Math.Max(1, (long)Math.Ceiling(ticks));
    }

    private static long ElapsedTicks(long start, long end) =>
        end > start ? end - start : 0;

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;
        Timer? timer = Interlocked.Exchange(ref _timer, null);
        if (timer != null)
        {
            using var callbacksDrained = new ManualResetEvent(false);
            if (timer.Dispose(callbacksDrained))
                callbacksDrained.WaitOne(TimeSpan.FromMilliseconds(500));
        }
        _transitionIdle.Wait(TimeSpan.FromMilliseconds(500));
    }
}
