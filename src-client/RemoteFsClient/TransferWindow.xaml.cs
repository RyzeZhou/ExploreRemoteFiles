using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Windows;
using RemoteFsClient.Services;

namespace RemoteFsClient;

/// <summary>
/// Transfer task window (uploads/downloads performed by the CLI). Shown
/// automatically when a job starts and hidden again shortly after every job
/// finishes; the instance is kept so the list survives hiding.
/// </summary>
public partial class TransferWindow : Window
{
    private readonly ObservableCollection<TransferTask> _tasks;

    public TransferWindow(ObservableCollection<TransferTask> tasks)
    {
        InitializeComponent();
        _tasks = tasks;
        TaskList.ItemsSource = tasks;
        ClearButton.Click += (_, _) =>
        {
            for (int i = _tasks.Count - 1; i >= 0; i--)
                if (_tasks[i].IsFinished) _tasks.RemoveAt(i);
        };
        CloseButton.Click += (_, _) => Hide();
    }

    /// <summary>Closing the window only hides it (the app owns the lifetime).</summary>
    protected override void OnClosing(CancelEventArgs e)
    {
        e.Cancel = true;
        Hide();
    }

    /// <summary>Number of jobs still running.</summary>
    public int RunningCount
    {
        get
        {
            int n = 0;
            foreach (TransferTask t in _tasks) if (!t.IsFinished) n++;
            return n;
        }
    }
}
