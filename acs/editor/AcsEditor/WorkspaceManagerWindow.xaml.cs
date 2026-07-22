// SPDX-License-Identifier: Apache-2.0

using System;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

namespace AcsEditor;

public partial class WorkspaceManagerWindow : Window
{
    private readonly EditorWorkspaceStore _store;
    private readonly EditorWorkspaceLayout _currentLayout;

    internal WorkspaceManagerWindow(
        EditorWorkspaceStore store,
        EditorWorkspaceLayout currentLayout)
    {
        _store = store;
        _currentLayout = EditorWorkspaceStore.NormalizeLayout(currentLayout);
        InitializeComponent();
        RefreshProfiles(_store.LastActiveName);
    }

    internal EditorWorkspaceProfile? SelectedProfile { get; private set; }

    private EditorWorkspaceProfile? CurrentSelection =>
        WorkspaceList.SelectedItem as EditorWorkspaceProfile;

    private void RefreshProfiles(string? selectName)
    {
        EditorWorkspaceProfile[] profiles = _store.GetProfiles().ToArray();
        WorkspaceList.ItemsSource = profiles;
        WorkspaceList.SelectedItem = profiles.FirstOrDefault(profile =>
            string.Equals(profile.Name, selectName, StringComparison.OrdinalIgnoreCase))
            ?? profiles.FirstOrDefault();
        UpdateActions();
    }

    private void UpdateActions()
    {
        EditorWorkspaceProfile? selected = CurrentSelection;
        bool hasSelection = selected != null;
        bool selectedUser = selected is { IsBuiltIn: false };
        bool hasName = !string.IsNullOrWhiteSpace(NameBox.Text);
        ActivateButton.IsEnabled = hasSelection;
        DuplicateButton.IsEnabled = hasSelection && hasName;
        RenameButton.IsEnabled = selectedUser && hasName;
        DeleteButton.IsEnabled = selectedUser;
        SaveCurrentButton.IsEnabled = hasName;
    }

    private void OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (CurrentSelection is { } selected)
            MessageText.Text = selected.IsBuiltIn
                ? "Built-in workspaces are read-only. Duplicate one to create a custom variant."
                : "User workspaces can be activated, renamed, replaced or deleted.";
        UpdateActions();
    }

    private void OnNameChanged(object sender, TextChangedEventArgs e) => UpdateActions();

    private void OnSaveCurrent(object sender, RoutedEventArgs e)
    {
        RunStoreAction(() =>
        {
            string name = NameBox.Text;
            bool exists = _store.TryGetProfile(name, out EditorWorkspaceProfile existing);
            if (exists && existing.IsBuiltIn)
                throw new InvalidOperationException("Built-in workspaces cannot be replaced.");
            if (exists && MessageBox.Show(
                    this,
                    $"Replace the saved workspace '{existing.Name}' with the current layout?",
                    "Replace Workspace",
                    MessageBoxButton.YesNo,
                    MessageBoxImage.Question) != MessageBoxResult.Yes)
                return;

            EditorWorkspaceProfile saved =
                _store.SaveUserProfile(name, _currentLayout, overwrite: exists);
            NameBox.Clear();
            RefreshProfiles(saved.Name);
            MessageText.Text = $"Saved '{saved.Name}' from the current editor layout.";
        });
    }

    private void OnDuplicate(object sender, RoutedEventArgs e)
    {
        if (CurrentSelection is not { } selected)
            return;
        RunStoreAction(() =>
        {
            EditorWorkspaceProfile duplicate =
                _store.DuplicateProfile(selected.Name, NameBox.Text);
            NameBox.Clear();
            RefreshProfiles(duplicate.Name);
            MessageText.Text = $"Duplicated '{selected.Name}' as '{duplicate.Name}'.";
        });
    }

    private void OnRename(object sender, RoutedEventArgs e)
    {
        if (CurrentSelection is not { IsBuiltIn: false } selected)
            return;
        RunStoreAction(() =>
        {
            EditorWorkspaceProfile renamed =
                _store.RenameUserProfile(selected.Name, NameBox.Text);
            NameBox.Clear();
            RefreshProfiles(renamed.Name);
            MessageText.Text = $"Renamed workspace to '{renamed.Name}'.";
        });
    }

    private void OnDelete(object sender, RoutedEventArgs e)
    {
        if (CurrentSelection is not { IsBuiltIn: false } selected)
            return;
        if (MessageBox.Show(
                this,
                $"Delete the workspace '{selected.Name}'?\n\nThis does not change the current editor layout.",
                "Delete Workspace",
                MessageBoxButton.YesNo,
                MessageBoxImage.Warning) != MessageBoxResult.Yes)
            return;

        RunStoreAction(() =>
        {
            _store.DeleteUserProfile(selected.Name);
            RefreshProfiles(_store.LastActiveName);
            MessageText.Text = $"Deleted '{selected.Name}'.";
        });
    }

    private void OnActivate(object sender, RoutedEventArgs e)
    {
        if (CurrentSelection is not { } selected)
            return;
        try
        {
            _store.MarkActive(selected.Name);
            SelectedProfile = selected.Clone();
            DialogResult = true;
        }
        catch (Exception ex)
        {
            MessageText.Text = ex.Message;
        }
    }

    private void RunStoreAction(Action action)
    {
        try
        {
            action();
        }
        catch (Exception ex)
        {
            MessageText.Text = ex.Message;
        }
    }

    private void OnTitleBarMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ChangedButton == MouseButton.Left)
            DragMove();
    }

    private void OnPreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key != Key.Escape)
            return;
        e.Handled = true;
        Close();
    }

    private void OnClose(object sender, RoutedEventArgs e) => Close();
}
