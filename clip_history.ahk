#Requires AutoHotkey v2.0
#SingleInstance Force
OnClipboardChange ClipChanged

ClipHistory.Init()

ClipChanged(type, text := A_Clipboard) {
    if !(type = 1 && StrLen(text)) || text == A_Tab
        return
    time := DateAdd(A_NowUTC, 9, "Hours")
    OldLength := ClipHistory._ClipHistory.Length
    ClipHistory.Nominate(text)
    ClipHistory.Push([time, text])
    ClipHistory.FileAppend(time, text)
    ClipHistory.Gap += (ClipHistory._ClipHistory.Length - OldLength)
    Tips("コピーしたよ")
}

class ClipHistory {
    static _ClipHistory := [], Filtered := [], Gap := 0, Path := "C:\Users\" A_UserName "\clip_history"

    static Init(acc := "") {
        for line in StrSplit(FileOpen(this.Path, "rw").Read(), "`n", "`r") {
            if line
                acc .= line
            else {
                if acc = ""
                    continue
                item := StrSplit(acc, "|")
                this._ClipHistory.Push([item[1], Base64Decode(item[2])])
                acc := ""
            }
        }
    }

    static ApplyFilter(keyword) {
        this.Filtered := keyword = "" ? this._ClipHistory.Clone() :
            Filter(this._ClipHistory, item => Instr(item[2], keyword))
    }

    static GetFocusItem(lv) {
        index := this.Filtered.Length - (lv.row * (lv.page - 1) + lv.GetNext()) + 1
        return this.Filtered[index]
    }

    static IndexOf(text) {
        for index, item in this._ClipHistory
            if item[2] = text
                return index
    }

    static Push(item) {
        this._ClipHistory.Push(item)
    }

    static Nominate(targetText, newText := "", index := 1, start := 1) {
        targetIndex := this.IndexOf(targetText)
        if !targetIndex
            return
        if StrLen(newText) {
            time := this._ClipHistory[targetIndex][1]
            this._ClipHistory[targetIndex][2] := newText
        } else
            this._ClipHistory.RemoveAt(targetIndex)
        Encoded := StrSplit(FileRead(this.Path), "`n", "`r")
        for end, line in Encoded {
            if line
                continue
            if index = targetIndex {
                Encoded.RemoveAt(start, end - start + 1)
                if StrLen(newText)
                    Encoded.InsertAt(start, time "|" Base64Encode(newText))
                FileOpen(this.Path, "w").Write(Join(Encoded, "`n"))
                return
            }
            index++
            start := end + 1
        }
    }

    static FileAppend(time, text) {
        FileAppend(time "|" Base64Encode(text) "`n", this.Path, "UTF-8")
    }
}

InitListView(row, height) {
    OnMessage(0x0006, (wParam, *) => wParam = 0 ? _Gui.Hide() : "")
    OnMessage(0x007B, (*) => 0)
    OnMessage(0x0100, WM_KEYDOWN)
    OnMessage(0x0102, WM_CHAR)
    OnMessage(0x020A, WM_MOUSEWHEEL)

    WM_KEYDOWN(wParam, lParam, msg, hwnd) {
        switch hwnd {
            case lv.Hwnd:
                switch wParam {
                    case 0x08: SendEvent("{BS}")
                        return 0
                    case 0x09: showEdit.Value := ""
                    case 0x0D: CopyItem(lv)
                    case 0x25: PageNext(lv, -1)
                    case 0x26: if (lv.GetNext() == 1) {
                        ChangeFocus(lv, -1)
                        return 0
                    }
                    case 0x27: PageNext(lv, 1)
                    case 0x28: if (lv.GetNext() == LVGetLength(lv)) {
                        ChangeFocus(lv, 1)
                        return 0
                    }
                    case 0x2E: SetTargetText(lv) ChangeItem(lv)
                }
            case filterEdit.Hwnd:
                switch wParam {
                    case 0x08: SendEvent("{BS}")
                    case 0x0D: CopyItem(lv)
                    case 0x26: ChangeFocus(lv, -1) lv.Focus()
                    case 0x28: ChangeFocus(lv, 1) lv.Focus()
                    default:
                        return ""
                }
                return 0
            case showEdit.Hwnd:
                switch wParam {
                    case 0x09: ((A_Clipboard := A_Tab) Send("^v"))
                        return 0
                    case 0x0D: if GetKeyState("Ctrl", "P") {
                        lv.Focus()
                        return 0
                    }
                }
        }
    }

    WM_CHAR(wParam, lParam, msg, hwnd) {
        if hwnd == lv.Hwnd {
            filterEdit.Focus()
            SendEvent("{End}" Chr(wParam))
            return 0
        }
    }

    WM_MOUSEWHEEL(wParam, lParam, msg, hwnd) {
        MouseGetPos(, , , &hCtrl, 2)
        if hCtrl == lv.Hwnd
            PageNext(lv, (wParam << 32 >> 48) > 0 ? -1 : 1)
    }

    static _Gui, lv, filterEdit, showEdit, theme := " cFFFFFF BackGround202020 "

    _Gui := Gui("+AlwaysOnTop -Caption")
    _Gui.BackColor := "202020"
    _Gui.OnEvent("Escape", (*) => isFocused(showEdit) ? lv.Focus() : _Gui.Hide())
    _Gui.OnEvent("Close", (*) => lv.Focus())

    filterEdit := _Gui.AddEdit(theme "w750 h34 vFilter -Vscroll -Tabstop -WantReturn")
    filterEdit.SetFont("s12", "Segoe UI")
    filterEdit.OnEvent("Change", (ctrl, *) => (ApplyFilter(lv, lv.GetNext())
        (lv.GetNext() ? "" : (showEdit.Value := ctrl.Value))))

    lv := _Gui.AddListView(theme "wp -Hdr -Tabstop h" 4 + (height + 1) * row, [""])
    lv.OnEvent("Focus", (*) => ShowFocusItem(lv))
    lv.OnEvent("ItemFocus", (*) => ShowFocusItem(lv))
    lv.OnEvent("DoubleClick", (*) => CopyItem(lv))

    showEdit := _Gui.AddEdit(theme "ym wp hp+42")
    showEdit.SetFont("s12", "Consolas")
    showEdit.OnEvent("Focus", (ctrl, *) => ((ctrl.Value := SetTargetText(lv))
        (lv.GetNext() ? "" : Send("{End}"))))
    showEdit.OnEvent("LoseFocus", (ctrl, *) => ChangeItem(lv, ctrl.Value))

    ImgListID := DllCall("ImageList_Create", "Int", 1, "Int", height, "UInt", 0x18, "Int", 1, "Int", 1)
    SendMessage(0x1003, 1, ImgListID, lv.Hwnd, "ahk_id " lv.Gui.Hwnd)
    return Assign(lv, { _Gui: _Gui, filterEdit: filterEdit, showEdit: showEdit, row: row })
}

ShowClipHistory() {
    static lv := InitListView(30, 18)
    try {
        Assign(lv, { page: 1, targetText: "" }).filterEdit.Value := ""
        ApplyFilter(lv)
        lv._Gui.Show()
        lv.Focus()
        WinSetTransParent(200, lv._Gui.Hwnd)
    }
}

isFocused := (guiObj) => ControlGetFocus("A") == guiObj.Hwnd

CopyItem := (lv) => ((A_Clipboard := GetFocusItem(lv)) lv._Gui.Hide())

SetTargetText := (lv) => (lv.targetText := GetFocusItem(lv))

Circulation := (cur, dir, len) => Mod(len + cur - 1 + Mod(dir, len), len) + 1

LVGetLength := (lv) =>
    Max(Min(ClipHistory.Filtered.Length - lv.row * (lv.page - 1), lv.row), 1)

ChangeFocus(lv, dir) {
    cur := lv.GetNext()
    len := LVGetLength(lv)
    row := Circulation(cur, dir, len)
    lv.Modify(cur, "-Focus -Select")
    lv.Modify(row, "Focus Select")
    ShowFocusItem(lv)
}

PageNext(lv, dir, targetRow := lv.GetNext()) {
    if lv.range = 1
        return
    lv.page := Circulation(lv.page, dir, lv.range)
    ShowFiltered(lv)
    lv.Modify(Max(Min(targetRow, LVGetLength(lv)), 1), "Focus Select")
    ShowFocusItem(lv)
}

GetFocusItem(lv) {
    try {
        return lv.GetNext() == 0 ? lv.filterEdit.Value : ClipHistory.GetFocusItem(lv)[2]
    } catch
        return ""
}

ChangeItem(lv, newText := "", targetRow := lv.GetNext(), time := 1) {
    try {
        if lv.targetText != newText {
            if targetRow && StrLen(lv.targetText) {
                ClipHistory.Nominate(lv.targetText, newText)
                lv.targetText := ""
                Tips((StrLen(newText) ? "保存" : "削除") "したよ")
            } else
                ((A_Clipboard := newText) (time := 100) ShowClipHistory())
        } else if ClipHistory.Gap == 0
            return
        SetTimer((*) => ApplyFilter(lv, targetRow + ClipHistory.Gap), -time)
    }
}

ApplyFilter(lv, targetRow := 1) {
    ClipHistory.Gap := 0
    ClipHistory.ApplyFilter(lv.FilterEdit.Value)
    lv.range := Max(Ceil(ClipHistory.Filtered.Length / lv.row), 1)
    lv.page := Min(lv.page, lv.range)
    ShowFiltered(lv)
    lv.Modify(Max(Min(targetRow, LVGetLength(lv)), 1), "Focus Select")
}

ShowFiltered(lv, end := ClipHistory.Filtered.Length - lv.row * (lv.page - 1)) {
    try {
        lv.Delete()
        lv.showEdit.Text := ""
        Filtered := Slice(ClipHistory.Filtered, Max(end - lv.row + 1, 1), end)
        Loop Filtered.Length
            lv.Add("", Filtered[Filtered.Length - A_Index + 1][2])
    }
}

ShowFocusItem(lv) {
    try {
        if isFocused(lv.showEdit)
            return
        item := ClipHistory.GetFocusItem(lv)
        lv.showEdit.Text :=
            "[" lv.row * (lv.page - 1) + lv.getNext() "/" ClipHistory.filtered.Length "] "
            . formatTime(item[1], "yyyy/MM/dd HH:mm:ss") "`r`n--`r`n"
            . StrReplace(item[2], "`n", "`r`n")
    }
}

Slice(arr, start, end, newArr := []) {
    Loop (Min(arr.Length, end) - Max(start, 1) + 1)
        newArr.Push(arr[start + A_Index - 1])
    return newArr
}

Filter(arr, fn, newArr := []) {
    for item in arr
        fn(item) ? newArr.Push(item) : ""
    return newArr
}

Mapcar(arr, fn, newArr := []) {
    for item in arr
        newArr.Push(fn(item))
    return newArr
}

Some(arr, fn) {
    for item in arr
        if fn(item) == true
            return true
    return false
}

Always(arr, fn) {
    for item in arr
        if fn(item) == false
            return false
    return true
}

Join(arr, sep := ",", str := "") {
    for index, param in arr
        str .= param (index = arr.Length ? "" : sep)
    return str
}

Assign(target, sources*) {
    for src in sources
        for key, value in src.OwnProps()
            target.%key% := value
    return target
}

Base64Encode(str) {
    bin := Buffer(StrPut(str, "UTF-8"))
    StrPut(str, bin, "UTF-8")
    return CryptBinaryToString(bin)
}

Base64Decode(b64) {
    bin := CryptStringToBinary(b64)
    return StrGet(bin, "UTF-8")
}

CryptBinaryToString(buf) {
    size := 0
    DllCall("Crypt32.dll\CryptBinaryToStringW", "Ptr", buf.Ptr, "UInt",
        buf.Size, "UInt", 1, "Ptr", 0, "UInt*", &size)
    out := Buffer(size * 2)
    DllCall("Crypt32.dll\CryptBinaryToStringW", "Ptr", buf.Ptr, "UInt",
        buf.Size, "UInt", 1, "Ptr", out.Ptr, "UInt*", &size)
    return StrGet(out)
}

CryptStringToBinary(str) {
    size := 0
    DllCall("Crypt32.dll\CryptStringToBinaryW", "Str", str, "UInt", 0, "UInt", 1,
        "Ptr", 0, "UInt*", &size, "Ptr", 0, "Ptr", 0)
    buf := Buffer(size)
    DllCall("Crypt32.dll\CryptStringToBinaryW", "Str", str, "UInt", 0, "UInt", 1,
        "Ptr", buf.Ptr, "UInt*", &size, "Ptr", 0, "Ptr", 0)
    return buf
}

Tips(msg, delay := 1000) => (ToolTip(msg) SetTimer(ToolTip, -delay))
Search(url) => (Send("^{c}") Sleep(200) Run(url A_Clipboard))

F15:: ShowClipHistory
F17:: Click
F18:: Send("+{MButton}")
F19:: Click("R")
F20:: Send("../")
F21:: Search("https://web.archive.org/web/")
F22:: Reload
#^+t:: {
    TransDegree := WinGetTransparent("A")
    TransDegree := TransDegree ? TransDegree : 256
    nextDegree := ""
    for i in [1, 2, 3, 4, 5, 6, 7, 8] {
        val := 256 - 32 * i
        if (TransDegree > val) {
            nextDegree := val ? val : "off"
            break
        }
    }
    WinSetTransparent(nextDegree, "A")
}

Tips("終わったよ", 800)
