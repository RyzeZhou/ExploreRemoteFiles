# -*- coding: utf-8 -*-
"""Service: add a 'current file' column to the transfer queue (WinSCP's
batch second-line semantics: the file currently in flight)."""
import io

# ============ 1. TransferTask: CurrentFile property ============
p = 'Services/TransferTaskService.cs'
s = io.open(p, encoding='utf-8').read()
n0 = s

old = '''    private bool _paused;
    private string _status = "running";'''
new = '''    private bool _paused;
    private string _status = "running";
    private string _currentFile = "";'''
assert old in s, 'fields'
s = s.replace(old, new, 1)

old = '''    public bool IsPaused => _paused;'''
new = '''    public bool IsPaused => _paused;

    /// <summary>For a multi-file (batch) job, the file currently being
    /// transferred. Empty for single-file jobs and after completion.</summary>
    public string CurrentFile
    {
        get => _currentFile;
        set { _currentFile = value ?? ""; Raise(nameof(CurrentFileText)); }
    }
    public string CurrentFileText => _currentFile;'''
assert old in s, 'IsPaused'
s = s.replace(old, new, 1)

# clear current file on finish
old = '''    public void Finish(bool ok, string message)
    {
        _status = ok ? "done" : "fail";
        if (ok && _total > 0) _done = _total;
        _speed = 0;'''
new = '''    public void Finish(bool ok, string message)
    {
        _status = ok ? "done" : "fail";
        if (ok && _total > 0) _done = _total;
        _speed = 0;
        _currentFile = "";'''
assert old in s, 'Finish'
s = s.replace(old, new, 1)
# Finish raises
old = '''        if (!ok && !string.IsNullOrWhiteSpace(message)) Failure = message;
        Raise(nameof(Percent));
        Raise(nameof(ProgressText));
        Raise(nameof(SpeedText));
        Raise(nameof(StatusText));
    }'''
new = '''        if (!ok && !string.IsNullOrWhiteSpace(message)) Failure = message;
        Raise(nameof(Percent));
        Raise(nameof(ProgressText));
        Raise(nameof(SpeedText));
        Raise(nameof(StatusText));
        Raise(nameof(CurrentFileText));
    }'''
assert old in s, 'Finish raises'
s = s.replace(old, new, 1)

# P frame: read the optional current-file field
old = '''                case "P" when f.Length >= 4:
                {
                    TransferTask? task = Find(f[1]);
                    if (task is null) break;
                    if (long.TryParse(f[2], out long done) && long.TryParse(f[3], out long total))
                        task.Update(done, total);
                    break;
                }'''
new = '''                case "P" when f.Length >= 4:
                {
                    TransferTask? task = Find(f[1]);
                    if (task is null) break;
                    if (long.TryParse(f[2], out long done) && long.TryParse(f[3], out long total))
                        task.Update(done, total);
                    if (f.Length >= 5) task.CurrentFile = f[4];
                    break;
                }'''
assert old in s, 'P frame'
s = s.replace(old, new, 1)

assert s != n0
io.open(p, 'w', encoding='utf-8').write(s)
print('TransferTaskService: CurrentFile OK')

# ============ 2. MainWindow.xaml: add column ============
p = 'MainWindow.xaml'
s = io.open(p, encoding='utf-8').read()
n0 = s
old = '''                                    <GridViewColumn x:Name="TColFile" Width="140" DisplayMemberBinding="{Binding Name}"/>'''
new = '''                                    <GridViewColumn x:Name="TColFile" Width="120" DisplayMemberBinding="{Binding Name}"/>
                                    <GridViewColumn x:Name="TColCurFile" Width="150" DisplayMemberBinding="{Binding CurrentFileText}"/>'''
assert old in s, 'TColFile'
s = s.replace(old, new, 1)
assert s != n0
io.open(p, 'w', encoding='utf-8').write(s)
print('MainWindow.xaml: current-file column OK')

# ============ 3. MainWindow.xaml.cs: localize header ============
p = 'MainWindow.xaml.cs'
s = io.open(p, encoding='utf-8').read()
n0 = s
old = '''            TColFile.Header = Ui.IsEnglish ? "File" : "文件";'''
new = '''            TColFile.Header = Ui.IsEnglish ? "File" : "文件";
            TColCurFile.Header = Ui.IsEnglish ? "Current file" : "当前文件";'''
assert old in s, 'TColFile header'
s = s.replace(old, new, 1)
assert s != n0
io.open(p, 'w', encoding='utf-8').write(s)
print('MainWindow.xaml.cs: header localized OK')
