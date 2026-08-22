using System.Runtime.InteropServices;
using ExplorerRemoteFs.Config;
using ExplorerRemoteFs.Providers;
using ExplorerRemoteFs.Shell.Interop;

namespace ExplorerRemoteFs.Shell;

/// <summary>
/// 自研 Shell Namespace 文件夹（IShellFolder2 完整实现，规范 docs/SHELL_NAMESPACE_SPEC.md）。
/// Win10 晚期 / Win11 兼容。
/// </summary>
[ComVisible(true)]
[Guid("9EADE55A-2AB6-4087-B5FF-D090D38173E6")]
[ClassInterface(ClassInterfaceType.None)]
public sealed class RemoteFsShellFolder : IShellFolder2, IPersistFolder2
{
    private static readonly Guid Clsid = new("9EADE55A-2AB6-4087-B5FF-D090D38173E6");
    private static readonly Guid IidShellView = new("000214E3-0000-0000-C000-000000000046");
    private static readonly Guid IidIShellFolder = new("000214E6-0000-0000-C000-000000000046");

    private readonly ConnectionConfig? _conn;
    private readonly string _path;
    private IntPtr _pidl = IntPtr.Zero;
    private bool _initialized;

    /// <summary>根节点（Servers）— COM 激活必需的无参构造。</summary>
    public RemoteFsShellFolder()
    {
        _conn = null;
        _path = "";
        Utils.ShellLog.Write("RemoteFsShellFolder ctor (root)");
    }

    /// <summary>连接或远程目录节点。</summary>
    public RemoteFsShellFolder(ConnectionConfig conn, string path)
    {
        _conn = conn;
        _path = path ?? "";
        Utils.ShellLog.Write($"RemoteFsShellFolder ctor: {conn.Name}:{path}");
    }

    private bool IsRoot => _conn is null;

    #region 内部逻辑

    private List<ItemInfo> GetChildrenItems()
    {
        try
        {
            if (IsRoot)
            {
                var conns = ConnectionStore.Load();
                Utils.ShellLog.Write($"Enum root: {conns.Count} connections");
                return conns.Select(ItemInfo.FromConnection).ToList();
            }

            var fs = ProviderFactory.Get(_conn!);
            var entries = fs.List(_path);
            Utils.ShellLog.Write($"Enum {_conn!.Name}:{_path} -> {entries.Count} items");
            return entries
                .Select(e => e.IsDirectory ? ItemInfo.FromDirectory(_conn!, e) : ItemInfo.FromFile(_conn!, e))
                .ToList();
        }
        catch (Exception ex)
        {
            Utils.ShellLog.Write($"GetChildrenItems failed: {ex.Message}");
            return new List<ItemInfo>();
        }
    }

    private ItemInfo? Resolve(string? id)
    {
        if (string.IsNullOrEmpty(id)) return null;
        try
        {
            if (IsRoot)
            {
                if (!id.StartsWith("c:", StringComparison.Ordinal)) return null;
                var name = id[2..];
                var conn = ConnectionStore.Load().FirstOrDefault(c => c.Name == name);
                return conn is null ? null : ItemInfo.FromConnection(conn);
            }

            if (id.StartsWith("d:", StringComparison.Ordinal))
            {
                var path = id[2..];
                return ItemInfo.FromDirectory(_conn!, new RemoteEntry { Name = NameOf(path), Path = path, IsDirectory = true });
            }
            if (id.StartsWith("f:", StringComparison.Ordinal))
            {
                var path = id[2..];
                return ItemInfo.FromFile(_conn!, new RemoteEntry { Name = NameOf(path), Path = path, IsDirectory = false });
            }
        }
        catch (Exception ex) { Utils.ShellLog.Write($"Resolve '{id}' failed: {ex.Message}"); }
        return null;
    }

    private static string NameOf(string path)
    {
        path = path.TrimEnd('/');
        var i = path.LastIndexOf('/');
        return i >= 0 ? path[(i + 1)..] : path;
    }

    #endregion

    #region IShellFolder

    public int ParseDisplayName(IntPtr hwnd, IntPtr pbc, string pszDisplayName, ref uint pchEaten, out IntPtr ppidl, ref SFGAO pdwAttributes)
    {
        ppidl = IntPtr.Zero;
        try
        {
            Utils.ShellLog.Write($"ParseDisplayName CALLED: '{pszDisplayName}' root={IsRoot}");
            ppidl = PidlHelper.Create(pszDisplayName);
            pchEaten = (uint)pszDisplayName.Length;
            return WinError.S_OK;
        }
        catch (Exception ex) { Utils.ShellLog.Write($"ParseDisplayName failed: {ex.Message}"); return WinError.E_UNEXPECTED; }
    }

    public int EnumObjects(IntPtr hwnd, SHCONTF grfFlags, out IEnumIDList ppenumIDList)
    {
        try
        {
            Utils.ShellLog.Write($"EnumObjects called (flags=0x{(uint)grfFlags:X}, root={IsRoot})");
            var items = GetChildrenItems();
            var pidls = items.Select(i => PidlHelper.Create(i.Id)).ToArray();
            ppenumIDList = new RemoteFsEnumIdList(pidls);
            return WinError.S_OK;
        }
        catch (Exception ex)
        {
            Utils.ShellLog.Write($"EnumObjects failed: {ex.Message}");
            ppenumIDList = null!;
            return WinError.E_UNEXPECTED;
        }
    }

    public int BindToObject(IntPtr pidl, IntPtr pbc, ref Guid riid, out IntPtr ppv)
    {
        ppv = IntPtr.Zero;
        try
        {
            Utils.ShellLog.Write($"BindToObject CALLED pidl={pidl} riid={riid}");
            var id = PidlHelper.GetLastId(pidl);
            if (id is null) return WinError.E_INVALIDARG;

            var item = Resolve(id);
            if (item is null) return WinError.E_INVALIDARG;
            if (!item.IsFolder) return WinError.E_NOINTERFACE;

            var subPath = item.Kind switch
            {
                ItemInfo.ItemKind.Connection => item.Connection!.StartPath,
                _ => item.Entry!.Path
            };
            var subFolder = new RemoteFsShellFolder(item.Connection!, subPath);
            ppv = Marshal.GetComInterfaceForObject(subFolder, typeof(IShellFolder));
            return WinError.S_OK;
        }
        catch (Exception ex)
        {
            Utils.ShellLog.Write($"BindToObject failed: {ex.Message}");
            return WinError.E_UNEXPECTED;
        }
    }

    public int BindToStorage(IntPtr pidl, IntPtr pbc, ref Guid riid, out IntPtr ppv)
    {
        ppv = IntPtr.Zero;
        return WinError.E_NOTIMPL;
    }

    public int CompareIDs(IntPtr lParam, IntPtr pidl1, IntPtr pidl2)
    {
        try
        {
            var id1 = PidlHelper.GetLastId(pidl1) ?? "";
            var id2 = PidlHelper.GetLastId(pidl2) ?? "";
            var f1 = id1.StartsWith("c:") || id1.StartsWith("d:");
            var f2 = id2.StartsWith("c:") || id2.StartsWith("d:");
            if (f1 != f2) return f1 ? -1 : 1;
            var cmp = string.CompareOrdinal(id1, id2);
            return cmp;
        }
        catch (Exception ex)
        {
            Utils.ShellLog.Write($"CompareIDs failed: {ex.Message}");
            return WinError.E_UNEXPECTED;
        }
    }

    public int CreateViewObject(IntPtr hwndOwner, ref Guid riid, out IntPtr ppv)
    {
        ppv = IntPtr.Zero;
        try
        {
            Utils.ShellLog.Write($"CreateViewObject CALLED riid={riid} root={IsRoot}");

            // 规范第 6 节：用 SHCreateShellFolderView + SFV_CREATE
            // 不手动 AddRef — .NET 在 CreateViewObject 期间持有 this 引用，
            // SHCreateShellFolderView 内部会 QI/AddRef pshf，DefView 持有引用
            // 对象在 .NET Release 后仍被 DefView 引用计数保活
            var unk = Marshal.GetIUnknownForObject(this);
            var sfv = new SFV_CREATE
            {
                cbSize = (uint)System.Runtime.InteropServices.Marshal.SizeOf<SFV_CREATE>(),
                pshf = unk,
                psvOuter = IntPtr.Zero,
                psfvc = IntPtr.Zero
            };

            var pViewPtr = Marshal.AllocCoTaskMem(IntPtr.Size);
            try
            {
                var hr = Shell32.SHCreateShellFolderView(sfv, pViewPtr);
                if (hr != WinError.S_OK) return hr;
                var viewPtr = Marshal.ReadIntPtr(pViewPtr);
                if (viewPtr == IntPtr.Zero) return WinError.E_UNEXPECTED;
                ppv = viewPtr;
                return WinError.S_OK;
            }
            finally
            {
                Marshal.FreeCoTaskMem(pViewPtr);
            }
        }
        catch (Exception ex)
        {
            Utils.ShellLog.Write($"CreateViewObject failed: {ex}");
            return WinError.E_UNEXPECTED;
        }
    }

    public int GetAttributesOf(uint cidl, IntPtr apidl, ref SFGAO rgfInOut)
    {
        try
        {
            Utils.ShellLog.Write($"GetAttributesOf CALLED cidl={cidl} root={IsRoot}");
            if (cidl == 0) return WinError.S_OK;
            var requested = rgfInOut;
            SFGAO common = (SFGAO)0xFFFFFFFF;
            for (uint i = 0; i < cidl; i++)
            {
                var pidl = Marshal.ReadIntPtr(apidl, (int)i * IntPtr.Size);
                var id = PidlHelper.GetLastId(pidl);
                var item = id is null ? null : Resolve(id);
                var attrs = item is { IsFolder: true }
                    ? SFGAO.SFGAO_FOLDER | SFGAO.SFGAO_HASSUBFOLDER | SFGAO.SFGAO_BROWSABLE | SFGAO.SFGAO_CANLINK
                    : SFGAO.SFGAO_CANCOPY | SFGAO.SFGAO_READONLY;
                common &= attrs;
            }
            rgfInOut = requested & common;
            return WinError.S_OK;
        }
        catch (Exception ex)
        {
            Utils.ShellLog.Write($"GetAttributesOf failed: {ex.Message}");
            return WinError.E_UNEXPECTED;
        }
    }

    public int GetUIObjectOf(IntPtr hwndOwner, uint cidl, IntPtr apidl, ref Guid riid, uint rgfReserved, out IntPtr ppv)
    {
        ppv = IntPtr.Zero;
        return WinError.E_NOTIMPL;
    }

    public int GetDisplayNameOf(IntPtr pidl, SHGDNF uFlags, out STRRET pName)
    {
        pName = default;
        try
        {
            Utils.ShellLog.Write($"GetDisplayNameOf CALLED root={IsRoot} flags={uFlags}");
            var id = PidlHelper.GetLastId(pidl);
            if (id is null) return WinError.E_INVALIDARG;
            var item = Resolve(id);
            if (item is null) return WinError.E_INVALIDARG;
            pName = STRRET.CreateUnicode(item.DisplayName);
            return WinError.S_OK;
        }
        catch (Exception ex)
        {
            Utils.ShellLog.Write($"GetDisplayNameOf failed: {ex.Message}");
            return WinError.E_UNEXPECTED;
        }
    }

    public int SetNameOf(IntPtr hwnd, IntPtr pidl, string pszName, SHGDNF uFlags, out IntPtr ppidlOut)
    {
        ppidlOut = IntPtr.Zero;
        return WinError.E_NOTIMPL;
    }

    #endregion

    #region IShellFolder2

    public int GetDefaultSearchGUID(out Guid pguid)
    {
        pguid = Guid.Empty;
        return WinError.E_NOTIMPL;
    }

    public int EnumSearches(out IEnumExtraSearch ppenum)
    {
        ppenum = null!;
        return WinError.E_NOTIMPL;
    }

    public int GetDefaultColumn(uint dwRes, out uint pSort, out uint pDisplay)
    {
        try
        {
            Utils.ShellLog.Write($"GetDefaultColumn CALLED root={IsRoot}");
            pSort = 0;
            pDisplay = 0;
            return WinError.S_OK;
        }
        catch (Exception ex) { Utils.ShellLog.Write($"GetDefaultColumn failed: {ex.Message}"); pSort = 0; pDisplay = 0; return WinError.E_UNEXPECTED; }
    }

    public int GetDefaultColumnState(uint iColumn, out SHCOLSTATEF pcsFlags)
    {
        try
        {
            Utils.ShellLog.Write($"GetDefaultColumnState CALLED iColumn={iColumn} root={IsRoot}");
            pcsFlags = SHCOLSTATEF.SHCOLSTATE_TYPE_STR | SHCOLSTATEF.SHCOLSTATE_ONBYDEFAULT;
            return WinError.S_OK;
        }
        catch (Exception ex) { Utils.ShellLog.Write($"GetDefaultColumnState failed: {ex.Message}"); pcsFlags = 0; return WinError.E_UNEXPECTED; }
    }

    public int GetDetailsEx(IntPtr pidl, PROPERTYKEY pscid, out object pv)
    {
        try
        {
            Utils.ShellLog.Write($"GetDetailsEx CALLED root={IsRoot}");
            pv = null!;
            return WinError.E_NOTIMPL;
        }
        catch (Exception ex) { Utils.ShellLog.Write($"GetDetailsEx failed: {ex.Message}"); pv = null!; return WinError.E_UNEXPECTED; }
    }

    public int GetDetailsOf(IntPtr pidl, uint iColumn, out SHELLDETAILS psd)
    {
        psd = default;
        try
        {
            Utils.ShellLog.Write($"GetDetailsOf CALLED pidl={pidl} iColumn={iColumn} root={IsRoot}");
            var cols = ColumnManager.GetColumns(IsRoot);
            if (iColumn >= (uint)cols.Length) return WinError.S_FALSE;

            psd = new SHELLDETAILS
            {
                fmt = 0,
                cxChar = cols[iColumn].Width,
                str = pidl == IntPtr.Zero
                    ? STRRET.CreateUnicode(cols[iColumn].Title)
                    : STRRET.CreateUnicode(GetColumnValue(pidl, iColumn))
            };
            return WinError.S_OK;
        }
        catch (Exception ex)
        {
            Utils.ShellLog.Write($"GetDetailsOf failed: {ex.Message}");
            return WinError.E_UNEXPECTED;
        }
    }

    private string GetColumnValue(IntPtr pidl, uint iColumn)
    {
        var id = PidlHelper.GetLastId(pidl);
        var item = id is null ? null : Resolve(id);
        if (item is null) return "";
        return ColumnManager.GetValue(item, iColumn, IsRoot);
    }

    public int MapColumnToSCID(uint iColumn, out PROPERTYKEY pscid)
    {
        pscid = default;
        return WinError.E_NOTIMPL;
    }

    #endregion

    #region IPersist / IPersistFolder / IPersistFolder2 / IPersistIDList

    public int GetClassID(out Guid pClassID)
    {
        try
        {
            Utils.ShellLog.Write($"GetClassID CALLED root={IsRoot}");
            pClassID = Clsid;
            return WinError.S_OK;
        }
        catch (Exception ex)
        {
            Utils.ShellLog.Write($"GetClassID failed: {ex.Message}");
            pClassID = Guid.Empty;
            return WinError.E_UNEXPECTED;
        }
    }

    public int Initialize(IntPtr pidl)
    {
        try
        {
            Utils.ShellLog.Write($"Initialize: pidl={pidl}, root={IsRoot}");
            if (_pidl != IntPtr.Zero) PidlHelper.Free(_pidl);
            _pidl = PidlHelper.Clone(pidl);
            _initialized = true;
            return WinError.S_OK;
        }
        catch (Exception ex)
        {
            Utils.ShellLog.Write($"Initialize failed: {ex.Message}");
            return WinError.E_UNEXPECTED;
        }
    }

    // 规范第 8 节：未初始化返回 S_FALSE + NULL；已初始化返回桌面绝对 PIDL
    public int GetCurFolder(out IntPtr ppidl)
    {
        try
        {
            var state = !_initialized ? "NOT-INIT" : (_pidl == IntPtr.Zero ? "PIDL-ZERO" : "OK");
            Utils.ShellLog.Write($"GetCurFolder CALLED state={state} _pidl={_pidl}");
            if (!_initialized || _pidl == IntPtr.Zero)
            {
                ppidl = IntPtr.Zero;
                return WinError.S_FALSE;
            }
            ppidl = PidlHelper.Clone(_pidl);
            var same = (ppidl == _pidl);
            Utils.ShellLog.Write($"  GetCurFolder returning Clone={ppidl} (same_as_input={same})");
            return WinError.S_OK;
        }
        catch (Exception ex)
        {
            Utils.ShellLog.Write($"GetCurFolder failed: {ex.Message}");
            ppidl = IntPtr.Zero;
            return WinError.E_UNEXPECTED;
        }
    }

    public int SetIDList(IntPtr pidl) => Initialize(pidl);

    public int GetIDList(out IntPtr pidl)
    {
        pidl = _pidl == IntPtr.Zero ? PidlHelper.Empty() : PidlHelper.Clone(_pidl);
        return WinError.S_OK;
    }

    #endregion
}
