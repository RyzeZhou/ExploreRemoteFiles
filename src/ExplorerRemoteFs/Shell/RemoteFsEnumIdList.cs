using System.Runtime.InteropServices;
using ExplorerRemoteFs.Shell.Interop;

namespace ExplorerRemoteFs.Shell;

/// <summary>
/// IEnumIDList 实现：枚举当前文件夹的子项 PIDL（规范第 5 节）。
/// </summary>
public sealed class RemoteFsEnumIdList : IEnumIDList
{
    private readonly IntPtr[] _pidls;
    private int _index;

    public RemoteFsEnumIdList(IEnumerable<IntPtr> pidls)
    {
        _pidls = pidls.ToArray();
    }

    public int Next(uint celt, IntPtr rgelt, out uint pceltFetched)
    {
        try
        {
            pceltFetched = 0;
            if (rgelt == IntPtr.Zero) return WinError.E_INVALIDARG;

            var count = (int)Math.Min(celt, (uint)(_pidls.Length - _index));
            for (var i = 0; i < count; i++)
                Marshal.WriteIntPtr(rgelt, i * IntPtr.Size, PidlHelper.Clone(_pidls[_index + i]));
            _index += count;
            pceltFetched = (uint)count;
            return count == (int)celt ? WinError.S_OK : WinError.S_FALSE;
        }
        catch (Exception ex)
        {
            Utils.ShellLog.Write($"EnumIdList.Next failed: {ex.Message}");
            pceltFetched = 0;
            return WinError.E_UNEXPECTED;
        }
    }

    public int Skip(uint celt)
    {
        _index = (int)Math.Min((uint)_pidls.Length, _index + celt);
        return _index >= _pidls.Length ? WinError.S_FALSE : WinError.S_OK;
    }

    public int Reset()
    {
        _index = 0;
        return WinError.S_OK;
    }

    public int Clone(out IEnumIDList ppenum)
    {
        ppenum = new RemoteFsEnumIdList(_pidls) { _index = _index };
        return WinError.S_OK;
    }
}
