using System;
using System.Globalization;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;

namespace AcsEditor;

/// <summary>
/// Reusable HSV/RGBA picker shared by material and component inspectors.
/// Call <see cref="TryPick"/> to keep callers independent from dialog internals.
/// </summary>
public partial class ColorPickerDialog : Window
{
    private readonly Color _original;
    private bool _syncing = true;
    private bool _draggingSv;
    private double _hue;
    private double _saturation;
    private double _value;
    private double _alpha;

    public Color SelectedColor { get; private set; }

    public ColorPickerDialog(Color initial, bool allowAlpha = true)
    {
        InitializeComponent();
        _original = initial;
        SelectedColor = initial;
        OriginalSwatch.Background = new SolidColorBrush(initial);
        AlphaRow.Visibility = allowAlpha ? Visibility.Visible : Visibility.Collapsed;
        AlphaBox.IsEnabled = allowAlpha;
        RgbToHsv(initial, out _hue, out _saturation, out _value);
        _alpha = initial.A / 255.0;
        HueSlider.Value = _hue;
        AlphaSlider.Value = _alpha;
        Loaded += (_, _) =>
        {
            _syncing = false;
            UpdateUiFromHsv();
        };
    }

    public static bool TryPick(Window? owner, Color initial, bool allowAlpha, out Color result)
    {
        var dialog = new ColorPickerDialog(initial, allowAlpha);
        if (owner != null) dialog.Owner = owner;
        bool accepted = dialog.ShowDialog() == true;
        result = accepted ? dialog.SelectedColor : initial;
        return accepted;
    }

    private void OnSaturationValueMouseDown(object sender, MouseButtonEventArgs e)
    {
        _draggingSv = true;
        SaturationValueSurface.CaptureMouse();
        SetSaturationValue(e.GetPosition(SaturationValueSurface));
    }

    private void OnSaturationValueMouseUp(object sender, MouseButtonEventArgs e)
    {
        if (!_draggingSv) return;
        SetSaturationValue(e.GetPosition(SaturationValueSurface));
        _draggingSv = false;
        SaturationValueSurface.ReleaseMouseCapture();
    }

    private void OnSaturationValueMouseMove(object sender, MouseEventArgs e)
    {
        if (!_draggingSv || e.LeftButton != MouseButtonState.Pressed) return;
        SetSaturationValue(e.GetPosition(SaturationValueSurface));
    }

    private void SetSaturationValue(Point p)
    {
        double width = Math.Max(1, SaturationValueSurface.ActualWidth);
        double height = Math.Max(1, SaturationValueSurface.ActualHeight);
        _saturation = Math.Clamp(p.X / width, 0, 1);
        _value = Math.Clamp(1 - p.Y / height, 0, 1);
        UpdateUiFromHsv();
    }

    private void OnHueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_syncing) return;
        _hue = e.NewValue >= 360 ? 0 : e.NewValue;
        UpdateUiFromHsv();
    }

    private void OnAlphaChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_syncing) return;
        _alpha = Math.Clamp(e.NewValue, 0, 1);
        UpdateUiFromHsv();
    }

    private void OnChannelTextChanged(object sender, System.Windows.Controls.TextChangedEventArgs e)
    {
        if (_syncing) return;
        if (!TryChannel(RedBox.Text, out byte r) ||
            !TryChannel(GreenBox.Text, out byte g) ||
            !TryChannel(BlueBox.Text, out byte b) ||
            !TryChannel(AlphaBox.Text, out byte a))
        {
            ValidationText.Text = "Channels must be integers from 0 to 255.";
            return;
        }
        ValidationText.Text = "";
        Color color = Color.FromArgb(a, r, g, b);
        RgbToHsv(color, out _hue, out _saturation, out _value);
        _alpha = a / 255.0;
        _syncing = true;
        HueSlider.Value = _hue;
        AlphaSlider.Value = _alpha;
        _syncing = false;
        UpdateUiFromHsv(updateChannels: false);
    }

    private void OnHexChanged(object sender, System.Windows.Controls.TextChangedEventArgs e)
    {
        if (_syncing) return;
        string value = (HexBox.Text ?? "").Trim().TrimStart('#');
        if (value.Length is not (6 or 8) ||
            !uint.TryParse(value, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out uint packed))
        {
            ValidationText.Text = "HEX must be RRGGBB or RRGGBBAA.";
            return;
        }
        byte r, g, b, a;
        if (value.Length == 6)
        {
            r = (byte)(packed >> 16); g = (byte)(packed >> 8); b = (byte)packed;
            a = (byte)Math.Round(_alpha * 255);
        }
        else
        {
            r = (byte)(packed >> 24); g = (byte)(packed >> 16);
            b = (byte)(packed >> 8); a = (byte)packed;
        }
        ValidationText.Text = "";
        Color color = Color.FromArgb(a, r, g, b);
        RgbToHsv(color, out _hue, out _saturation, out _value);
        _alpha = a / 255.0;
        _syncing = true;
        HueSlider.Value = _hue;
        AlphaSlider.Value = _alpha;
        _syncing = false;
        UpdateUiFromHsv();
    }

    private void OnHsvTextChanged(object sender, System.Windows.Controls.TextChangedEventArgs e)
    {
        if (_syncing) return;
        if (!double.TryParse(HueBox.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out double h) ||
            !double.TryParse(SaturationBox.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out double s) ||
            !double.TryParse(ValueBox.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out double v) ||
            !double.IsFinite(h) || !double.IsFinite(s) || !double.IsFinite(v) ||
            h < 0 || h > 360 || s < 0 || s > 100 || v < 0 || v > 100)
        {
            ValidationText.Text = "HSV ranges: H 0-360, S/V 0-100.";
            return;
        }
        ValidationText.Text = "";
        _hue = h >= 360 ? 0 : h;
        _saturation = s / 100.0;
        _value = v / 100.0;
        _syncing = true;
        HueSlider.Value = _hue;
        _syncing = false;
        UpdateUiFromHsv(updateHsv: false);
    }

    private static bool TryChannel(string text, out byte value)
    {
        if (byte.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out value))
            return true;
        value = 0;
        return false;
    }

    private void UpdateUiFromHsv(bool updateChannels = true, bool updateHsv = true)
    {
        Color opaque = HsvToRgb(_hue, _saturation, _value, 255);
        SelectedColor = Color.FromArgb((byte)Math.Round(_alpha * 255), opaque.R, opaque.G, opaque.B);
        Color pureHue = HsvToRgb(_hue, 1, 1, 255);
        HueSurface.Background = new SolidColorBrush(pureHue);
        AlphaStart.Color = Color.FromArgb(0, opaque.R, opaque.G, opaque.B);
        AlphaEnd.Color = Color.FromArgb(255, opaque.R, opaque.G, opaque.B);
        CurrentSwatch.Background = new SolidColorBrush(SelectedColor);

        double width = Math.Max(1, SaturationValueSurface.ActualWidth);
        double height = Math.Max(1, SaturationValueSurface.ActualHeight);
        System.Windows.Controls.Canvas.SetLeft(SaturationValueThumb, _saturation * width - 7);
        System.Windows.Controls.Canvas.SetTop(SaturationValueThumb, (1 - _value) * height - 7);

        _syncing = true;
        if (updateChannels)
        {
            RedBox.Text = SelectedColor.R.ToString(CultureInfo.InvariantCulture);
            GreenBox.Text = SelectedColor.G.ToString(CultureInfo.InvariantCulture);
            BlueBox.Text = SelectedColor.B.ToString(CultureInfo.InvariantCulture);
            AlphaBox.Text = SelectedColor.A.ToString(CultureInfo.InvariantCulture);
        }
        if (updateHsv)
        {
            HueBox.Text = _hue.ToString("0.##", CultureInfo.InvariantCulture);
            SaturationBox.Text = (_saturation * 100).ToString("0.##", CultureInfo.InvariantCulture);
            ValueBox.Text = (_value * 100).ToString("0.##", CultureInfo.InvariantCulture);
        }
        HexBox.Text = $"#{SelectedColor.R:X2}{SelectedColor.G:X2}{SelectedColor.B:X2}{SelectedColor.A:X2}";
        _syncing = false;
    }

    private static Color HsvToRgb(double hue, double saturation, double value, byte alpha)
    {
        hue = ((hue % 360) + 360) % 360;
        saturation = Math.Clamp(saturation, 0, 1);
        value = Math.Clamp(value, 0, 1);
        double chroma = value * saturation;
        double x = chroma * (1 - Math.Abs((hue / 60) % 2 - 1));
        double m = value - chroma;
        (double r, double g, double b) rgb = hue switch
        {
            < 60 => (chroma, x, 0.0),
            < 120 => (x, chroma, 0.0),
            < 180 => (0.0, chroma, x),
            < 240 => (0.0, x, chroma),
            < 300 => (x, 0.0, chroma),
            _ => (chroma, 0.0, x)
        };
        return Color.FromArgb(alpha,
            (byte)Math.Round((rgb.r + m) * 255),
            (byte)Math.Round((rgb.g + m) * 255),
            (byte)Math.Round((rgb.b + m) * 255));
    }

    private static void RgbToHsv(Color color, out double hue, out double saturation, out double value)
    {
        double r = color.R / 255.0, g = color.G / 255.0, b = color.B / 255.0;
        double max = Math.Max(r, Math.Max(g, b));
        double min = Math.Min(r, Math.Min(g, b));
        double delta = max - min;
        if (delta <= 1e-9) hue = 0;
        else if (max == r) hue = 60 * (((g - b) / delta) % 6);
        else if (max == g) hue = 60 * ((b - r) / delta + 2);
        else hue = 60 * ((r - g) / delta + 4);
        if (hue < 0) hue += 360;
        saturation = max <= 1e-9 ? 0 : delta / max;
        value = max;
    }

    private void OnAccept(object sender, RoutedEventArgs e)
    {
        DialogResult = true;
        Close();
    }

    private void OnCancel(object sender, RoutedEventArgs e)
    {
        SelectedColor = _original;
        DialogResult = false;
        Close();
    }

    private void OnPreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Escape) OnCancel(sender, e);
        else if (e.Key == Key.Enter && Keyboard.Modifiers == ModifierKeys.Control) OnAccept(sender, e);
    }
}
