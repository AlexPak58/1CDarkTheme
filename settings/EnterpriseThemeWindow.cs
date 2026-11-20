using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Interop;

namespace ConfiguratorThemeSettings;

internal sealed class EnterpriseThemeWindow
{
    private const int CopyDataMessage = 0x004A;
    private const int ThemeTarget = 0x31435447;
    private const int ThemeProfile = 0x31435046;
    private readonly nint _owner;
    private nint _target;
    private int _targetPid;
    internal bool IsDesignerTarget { get; private set; }
    internal Preset? WindowPreset { get; private set; }

    [StructLayout(LayoutKind.Sequential)]
    private struct CopyData
    {
        public nint Kind;
        public int Size;
        public nint Bytes;
    }

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(nint window, out int process);

    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW")]
    private static extern nint SendMessageTimeout(nint window, int message, nint sender,
        ref CopyData data, uint flags, uint timeout, out nint result);

    internal EnterpriseThemeWindow(nint owner, nint initialTarget, bool designerTarget = false)
    {
        _owner = owner;
        HwndSource.FromHwnd(owner)?.AddHook(Receive);
        if (initialTarget != 0) SetTarget(initialTarget, null, designerTarget, null);
    }

    private nint Receive(nint window, int message, nint sender, nint argument, ref bool handled)
    {
        if (message != CopyDataMessage || argument == 0) return 0;
        var data = Marshal.PtrToStructure<CopyData>(argument);
        const int designerBytes = (4 + Palette.ColorCount) * sizeof(int);
        if (data.Kind != ThemeTarget || (data.Size != sizeof(int) && data.Size != designerBytes) || data.Bytes == 0) return 0;
        handled = true;
        var designer = data.Size == designerBytes;
        Preset? current = null;
        if (designer)
        {
            var words = new int[4 + Palette.ColorCount];
            Marshal.Copy(data.Bytes,words,0,words.Length);
            if (words[1] != designerBytes - sizeof(int) || words[2] != Palette.Version ||
                words[3] < 0 || words[3] > Palette.AllZones) return 0;
            var colors = new string[Palette.ColorCount];
            for (var index = 0; index < colors.Length; index++)
            {
                if (words[index + 4] is < 0 or > 0xFFFFFF) return 0;
                colors[index] = "#" + words[index + 4].ToString("X6",CultureInfo.InvariantCulture);
            }
            current = new Preset("",words[3],colors);
        }
        return SetTarget(sender,Marshal.ReadInt32(data.Bytes),designer,current) ? 1 : 0;
    }

    private bool SetTarget(nint target, int? expectedPid, bool designer, Preset? current)
    {
        if (GetWindowThreadProcessId(target, out var pid) == 0 || (expectedPid.HasValue && pid != expectedPid)) return false;
        try
        {
            using var process = Process.GetProcessById(pid);
            var path = process.MainModule?.FileName;
            if (path is null || Path.GetFileName(path) is not ("1cv8.exe" or "1cv8c.exe") ||
                (designer && Path.GetFileName(path) != "1cv8.exe")) return false;
        }
        catch (Exception error) when (error is ArgumentException or InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            return false;
        }
        _target = target;
        _targetPid = pid;
        IsDesignerTarget = designer;
        WindowPreset = current;
        return true;
    }

    internal bool? Apply(Preset preset)
    {
        if (_target == 0) return null;
        if (GetWindowThreadProcessId(_target, out var pid) == 0 || pid != _targetPid)
        {
            ClearTarget();
            return null;
        }
        var words = new uint[3 + Palette.ColorCount];
        words[0] = (uint)(words.Length * sizeof(uint));
        words[1] = Palette.Version;
        words[2] = (uint)preset.Mask;
        for (var index = 0; index < preset.Colors.Length; index++)
            words[3 + index] = uint.Parse(preset.Colors[index].AsSpan(1), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
        var pin = GCHandle.Alloc(words, GCHandleType.Pinned);
        try
        {
            var data = new CopyData { Kind = ThemeProfile, Size = (int)words[0], Bytes = pin.AddrOfPinnedObject() };
            var applied = SendMessageTimeout(_target, CopyDataMessage, _owner, ref data, 0x0003, 1500, out var result) != 0 && result == 1;
            if (applied && IsDesignerTarget) WindowPreset = preset;
            return applied;
        }
        finally { pin.Free(); }
    }

    internal void ClearTarget()
    {
        _target = 0;
        IsDesignerTarget = false;
        WindowPreset = null;
    }
}
