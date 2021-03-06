#include "StdAfx.h"
#include "vst.h"
#include "resource.h"
#include "editor.h"
#include "shared.h"
#include "Helpers/InputBox.h"

using namespace stringcvt;

static const GUID guid_show_presets =
	{ 0xceb12fbb, 0xff11, 0x4ae1,
	{ 0xb7, 0xf2, 0xb0, 0x8, 0x49, 0x5c, 0xb, 0x8c } };
static advconfig_checkbox_factory_t<false> cfg_show_presets(
	"Always show Presets Pane",
	guid_show_presets, guid_adv_vst, 0.0, false);

const wchar_t* class_name = L"VSTEditorHost";
static bool cust_control_init = false;

LRESULT editor::hook_proc(int p_ncode, WPARAM p_wp, LPARAM p_lp)
{
	for (const_iterator<editor*> i = instanceList().first();
		i.is_valid(); i.next())
	{
		// bit 30 is prev key state
		if ((*i)->m_threadid == GetCurrentThreadId() && p_lp & 1 << 30)
		{
			(*i)->SendDlgItemMessageW(IDC_VSTEDITOR, WM_KEYDOWN, p_wp, p_lp);
			break;
		}
	}
	return CallNextHookEx(NULL, p_ncode, p_wp, p_lp);
}

LRESULT CALLBACK CustWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_KEYDOWN || msg == WM_KEYUP)
	{
		if (wParam == VK_ESCAPE && msg == WM_KEYUP) SendMessage(GetForegroundWindow(), WM_CLOSE, 0, 0);
		SendMessage(GetWindow(hwnd, GW_CHILD), msg, wParam, lParam);
	}
	switch(msg)
	{
		case WM_MOUSEACTIVATE:
			SetFocus(hwnd);
			return MA_ACTIVATE;
		default:
			break;
	}
	return DefWindowProc(hwnd, msg, wParam, lParam);
}

editor::editor(
	const dsp_preset & p_preset, const entry & p_entry,
	dsp_preset_edit_callback & p_callback, vst * p_vst) :
		m_initial(p_preset), m_callback(p_callback), m_edited(false),
		m_vst(p_vst), m_prests_loaded(false)
{
	m_effect = m_vst->get_effect();
	m_quithandle = CreateEvent(NULL, FALSE, TRUE, L"DirChangeWatchingTerminator");

	if (!cust_control_init)
	{
		WNDCLASSEX wc;
		{
			wc.cbSize         = sizeof(wc);
			wc.lpszClassName  = class_name;
			wc.hInstance      = core_api::get_my_instance();
			wc.lpfnWndProc    = CustWndProc;
			wc.hCursor        = LoadCursor(NULL, IDC_ARROW);
			wc.hIcon          = 0;
			wc.lpszMenuName   = 0;
			wc.hbrBackground  = (HBRUSH)GetSysColorBrush(COLOR_BTNFACE);
			wc.style          = 0;
			wc.cbClsExtra     = 0;
			wc.cbWndExtra     = 0;
			wc.hIconSm        = 0;
		}
		RegisterClassEx(&wc);
		cust_control_init = true;
	}
}

editor::~editor(void)
{
}

LRESULT editor::on_timer(UINT, WPARAM wParam, LPARAM, BOOL&)
{
	if (wParam == 0)
	{
		m_effect->dispatcher(m_effect, effEditIdle, 0, 0, NULL, 0.0f);
	}
	return TRUE;
}

LRESULT editor::deltapos(int wParam, LPNMHDR lParam, BOOL &bHandled)
{
	update_lim_display(m_updown->GetPos());
	bHandled = FALSE;
	return TRUE;
}

void editor::update_lim_display(int val)
{
	static const wchar_t* fmt = L"<a>%d</a>";
	static const wchar_t* sauto = L"<a>A</a>";
	wchar_t caption[40] = {0};
	wsprintf(caption, fmt, val);
	if (val > 0) GetDlgItem(IDC_OUTINFO).SetWindowTextW(caption);
	else if (val == 0) GetDlgItem(IDC_OUTINFO).SetWindowTextW(sauto);
	m_vst->set_out_lim(val);
}

void editor::on_command(UINT id, CPoint)
{
	SetMsgHandled(false);
	if (id == effSetChunk)
	{
		string8 filename;
		if (uGetOpenFileName(m_hWnd, get_rsrc_string(IDS_FXPMASK), 0, "fxp",
			"", NULL, filename, false))
		{
			m_vst->load_fxp(filename);
		}
	}
	else if (id == effGetChunk)
	{
		char title[28] = {0};
		m_effect->dispatcher(m_effect, effGetProgramName, 0, 0, title, 0.0f);
		string8 filename;
		if (uGetOpenFileName(m_hWnd, get_rsrc_string(IDS_FXPMASK), 0, "fxp",
			stringcvt::string_utf8_from_ansi(title), NULL, filename, true))
		{
			m_vst->store_fxp(filename);
		}
	}
}

void editor::on_key_down(UINT p_char, UINT p_count, UINT p_flags)
{
}

BOOL editor::on_init_dialog(CWindow, LPARAM)
{
	CButton bypass(GetDlgItem(IDC_BYPASS));
	bypass.SetCheck(m_vst->get_bypass() ? BST_CHECKED : BST_UNCHECKED);
	CButton checkbox(GetDlgItem(IDC_VIEWPRESETS));
	checkbox.SetCheck(cfg_show_presets.get() ? BST_CHECKED : BST_UNCHECKED);
	m_presets = new CListBox(GetDlgItem(IDC_PRESETLIST));
	{
		m_updown = new CUpDownCtrl(GetDlgItem(IDC_OUTNSPIN));
		m_updown->SetRange32(0, m_effect->numOutputs);
		m_updown->SetBase(1);
		m_updown->SetPos(m_vst->get_out_lim());
		update_lim_display(m_vst->get_out_lim());
	}
	m_effect->resvd1 = reinterpret_cast<VstIntPtr>(this);
	InsertMenu(GetSystemMenu(FALSE),0, MF_STRING, effSetChunk, L"&Load FXP...");
	InsertMenu(GetSystemMenu(FALSE),0, MF_STRING, effGetChunk, L"&Save FXP...");
	SetWindowText(stringcvt::string_os_from_utf8(m_vst->get_product()));
	// Move to the main window if it's visible
	HWND mainwnd = core_api::get_main_window();
	if (::IsWindowVisible(mainwnd) &&
		!(::GetWindowLong(mainwnd, GWL_STYLE) & WS_MINIMIZE))
	{
		RECT r = {0};
		::GetWindowRect(mainwnd, &r);
		MoveWindow(&r, false);
	}
	m_effect->dispatcher(
		m_effect, effEditOpen, 0, 0, GetDlgItem(IDC_VSTEDITOR).m_hWnd, 0.0f);
	ERect* rect = NULL;
	m_effect->dispatcher(m_effect, effEditGetRect, 0, 0, &rect, 0.0f);
	resize_editor(rect->right - rect->left, rect->bottom - rect->top);
	SetTimer(0, 30, 0);
	m_threadid = GetCurrentThreadId();
	m_hook = SetWindowsHookEx(WH_KEYBOARD, hook_proc,
		core_api::get_my_instance(), m_threadid);

	/*CTrackBarCtrl* tr = new CTrackBarCtrl();
	RECT r = {100, 0, 200, 20};
	tr->Create(m_hWnd, r, CTrackBarCtrl::GetWndClassName(),
                          WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | 
                          WS_CLIPCHILDREN, 0);*/
	//tr->MoveWindow(50, 10, 200, 20);

	return TRUE;
}

BOOL editor::on_close()
{
	KillTimer(0);
	SetMsgHandled(false);
	return EndDialog(1);
}

void editor::on_destroy()
{
	SetEvent(m_quithandle);
	SetMsgHandled(false);
	m_effect->dispatcher(m_effect, effEditClose, 0, 0, NULL, 0.0f);
	save_config();
	m_effect->resvd1 = NULL;
	delete m_updown;
	delete m_presets;
	CloseHandle(m_quithandle);
	UnhookWindowsHookEx(m_hook);
}

void editor::save_config()
{
	//if (m_edited) // :TODO: Some plug-ins don't call automate?
	if (true)
	{
		dsp_preset_impl preset;
		m_vst->get_preset(m_initial.get_owner(), preset);
		m_vst->upd_preset(preset);
		m_callback.on_preset_changed(preset);
		m_initial.copy(preset);
	}
}

void editor::on_viewpresets(UINT, int, ATL::CWindow)
{
	// Just refresh it
	ERect* rect = NULL;
	m_effect->dispatcher(m_effect, effEditGetRect, 0, 0, &rect, 0.0f);
	resize_editor(rect->right - rect->left, rect->bottom - rect->top);
}

void editor::on_bypass(UINT, int, ATL::CWindow)
{
	CButton bypass(GetDlgItem(IDC_BYPASS));
	m_vst->set_bypass(bypass.GetCheck() == BST_CHECKED);
}

void editor::resize_editor(int w, int h)
{
	CWindow ed = GetDlgItem(IDC_VSTEDITOR);
	RECT r = {0};
	ed.GetWindowRect(&r);
	ScreenToClient(&r);
	ed.ResizeClient(w, h);
	CButton btnadd(GetDlgItem(IDC_BTNADDPRESET));
	CButton btndel(GetDlgItem(IDC_BTNDELPRESETS2));
	CStatic link(GetDlgItem(IDC_OPENFOLDER));
	CStatic builtin(GetDlgItem(IDC_TXTBUILTIN));
	CButton checkbox(GetDlgItem(IDC_VIEWPRESETS));
	//CButton bypass(GetDlgItem(IDC_BYPASS));
	RECT r_prs = {0};
	RECT r_add = {0};
	RECT r_del = {0};
	RECT r_lnk = {0};
	RECT r_vpr = {0};
	RECT r_blt = {0};
	//RECT r_bps = {0};
	m_presets->GetWindowRect(&r_prs);
	btnadd.GetWindowRect(&r_add);
	btndel.GetWindowRect(&r_del);
	link.GetWindowRect(&r_lnk);
	checkbox.GetWindowRect(&r_vpr);
	//bypass.GetWindowRect(&r_bps);
	builtin.GetWindowRect(&r_blt);
	ScreenToClient(&r_prs);
	ScreenToClient(&r_add);
	ScreenToClient(&r_del);
	ScreenToClient(&r_lnk);
	ScreenToClient(&r_vpr);
	ScreenToClient(&r_blt);
	//ScreenToClient(&r_bps);
	if (checkbox.GetCheck() == BST_CHECKED)
	{
		m_presets->MoveWindow(w + 8, r_prs.top, r_prs.right - r_prs.left,
			h - (r_lnk.bottom - r_lnk.top) - (r_add.bottom - r_add.top) - 6);
		btnadd.MoveWindow(w + 7, r_add.top,
			r_add.right - r_add.left, r_add.bottom - r_add.top);
		btndel.MoveWindow(w + 7 + 3 + r_add.right - r_add.left, r_del.top,
			r_del.right - r_del.left, r_del.bottom - r_del.top);
		m_presets->GetWindowRect(&r_prs);
		ScreenToClient(&r_prs);
		link.MoveWindow(w + 8, r_prs.bottom, r_lnk.right - r_lnk.left,
			r_lnk.bottom - r_lnk.top);
		m_presets->GetWindowRect(&r_prs);
		ScreenToClient(&r_prs);
		builtin.MoveWindow(
			r_prs.right - (r_blt.right - r_blt.left),
			r_prs.top - (r_blt.bottom - r_blt.top),
			r_blt.right - r_blt.left, r_blt.bottom - r_blt.top);
		// First move, then show. It flickers otherwise.
		btnadd.ShowWindow(SW_SHOW);
		btndel.ShowWindow(SW_SHOW);
		m_presets->ShowWindow(SW_SHOW);
		link.ShowWindow(SW_SHOW);
		builtin.ShowWindow(SW_SHOW);
		ResizeClient(w + 14 + r_prs.right - r_prs.left, h + r.top, true);
		if (!m_prests_loaded)
		{
			m_prests_loaded = true;
			load_presets();
			ResetEvent(m_quithandle);
			CloseHandle(CreateThread(NULL, 0, watch_dir, this, 0, 0));
		}
	}
	else if (checkbox.GetCheck() == BST_UNCHECKED)
	{
		btnadd.ShowWindow(SW_HIDE);
		btndel.ShowWindow(SW_HIDE);
		m_presets->ShowWindow(SW_HIDE);
		link.ShowWindow(SW_HIDE);
		builtin.ShowWindow(SW_HIDE);
		ResizeClient(w, h + r.top, true);
	}
	checkbox.MoveWindow(w - (r_vpr.right - r_vpr.left), r_vpr.top,
			r_vpr.right - r_vpr.left, r_vpr.bottom - r_vpr.top);
	/*bypass.MoveWindow(w - (r_vpr.right - r_vpr.left) - (r_bps.right - r_bps.left) - 5,
		r_bps.top, r_bps.right - r_bps.left, r_bps.bottom - r_bps.top);*/
}

void editor::load_presets()
{
	// Clean up
	while (m_presets->GetCount() > 0) m_presets->DeleteString(0);
	// Get the product string and make it safe for use in filenames
	string prd_unic = m_vst->get_product();
	// "/Supadupa?:Vst\"
	string8 prd_safe;
	for (int i = 0, j = prd_unic.get_length(); i < j; i++)
	{
		if (!is_path_bad_char(prd_unic[i]))
		{
			prd_safe.add_char(prd_unic[i]);
		}
	}
	// "/SupadupaVst\"
	prd_safe.replace_char('\\', '_');
	prd_safe.replace_char('/', '_');
	// "SupadupaVst"
	string8 dir;
	dir << core_api::get_profile_path();
	dir.remove_chars(0, 7); // remove "file://"
	dir << "\\vst-presets\\";
	CreateDirectory(string_os_from_utf8(dir), NULL);
	dir << prd_safe;
	dir << "\\";
	m_dir = dir;
	CreateDirectory(string_os_from_utf8(dir), NULL);
	dir << "*.fxp";
	WIN32_FIND_DATA ffd = {0};
	BOOL r = TRUE;
	HANDLE h = FindFirstFile(string_os_from_utf8(dir), &ffd);
	while(h != INVALID_HANDLE_VALUE && r != FALSE)
	{
		if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		{
			PathRemoveExtension(ffd.cFileName);
			m_presets->AddString(ffd.cFileName);
		}
		r = FindNextFile(h, &ffd);
	}
	FindClose(h);
	for (int i = 0; i < m_effect->numPrograms; i++)
	{
		char buf[(kVstMaxProgNameLen * 2) + 2] = {0}; // x2 just in case
		buf[0] = '*'; buf[1] = ' ';
		if (m_effect->dispatcher(
			m_effect, effGetProgramNameIndexed, i, 0, buf + 2, 0.0f) == TRUE)
		{
			m_presets->AddString(string_wide_from_ansi(buf));
			m_presets->SetItemData(m_presets->GetCount() - 1, (DWORD_PTR)i);
		}
	}
}

DWORD __stdcall editor::watch_dir(void* p_arg)
{
	editor* _this = static_cast<editor*>(p_arg);
	string_os_from_utf8 dir(_this->m_dir);
	HANDLE h_dirchanged = FindFirstChangeNotification(dir, FALSE,
			FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_FILE_NAME);
	if (h_dirchanged == INVALID_HANDLE_VALUE || h_dirchanged == NULL)
	{
		return TRUE;
	}
	const HANDLE handles[] = {_this->m_quithandle, h_dirchanged};
	while (true)
	{
		DWORD status = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
		if (status == WAIT_OBJECT_0 || status == WAIT_TIMEOUT) break;
		else if (status == WAIT_OBJECT_0 + 1)
		{
			_this->load_presets();
			if (FindNextChangeNotification(h_dirchanged) == 0) break;
		}
	}
	FindCloseChangeNotification(h_dirchanged);
	return TRUE;
}

LRESULT editor::open_folder(int wParam, LPNMHDR lParam, BOOL &bHandled)
{
	string_os_from_utf8 thedir(m_dir);
	ITEMIDLIST *dir = ILCreateFromPath(thedir);
	ITEMIDLIST** items = new ITEMIDLIST*[m_presets->GetSelCount()];
	int count = 0;
	for (int i = 0; i < m_presets->GetCount(); i++)
	{
		wchar_t txt[MAX_PATH] = {0};
		m_presets->GetText(i, txt);
		if (txt[0] != L'*' && m_presets->GetSel(i) > 0) // Built-in preset
		{
			wchar_t* fname = new wchar_t[m_dir.length() + MAX_PATH];
			wsprintf(fname, L"%s%s.fxp", thedir.get_ptr(), txt);
			ITEMIDLIST* item = ILCreateFromPath(fname);
			items[count] = item;
			delete fname;
			count++;
		}
	}

	SHOpenFolderAndSelectItems(dir, count, const_cast<LPCITEMIDLIST*>(items), 0);
	//Free resources
	ILFree(dir);
	for (int i = 0; i < count; i++) ILFree(items[i]);
	delete items;
	return TRUE;
}

void editor::on_listdblclick(UINT, int, ATL::CWindow)
{
	int item = 0;
	m_presets->GetSelItems(1, &item);
	wchar_t txt[MAX_PATH] = {0};
	m_presets->GetText(item, txt);
	if (txt[0] == L'*') // Built-in preset
	{
		m_effect->dispatcher(m_effect, effSetProgram, 0,
			m_presets->GetItemData(item), NULL, 0.0f);
	}
	else // File preset
	{
		string8 fxp = m_dir;
		fxp << string_utf8_from_os(txt) << ".fxp";
		m_vst->load_fxp(fxp);
	}
}

LRESULT editor::outinfo(int wParam, LPNMHDR lParam, BOOL &bHandled)
{
	popup_message::g_show(get_rsrc_string(IDS_OUTINFO), "Information");
	return TRUE;
}

void editor::on_add(UINT, int, ATL::CWindow)
{
	CInputBox ibox(m_hWnd);
	char prog[kVstMaxProgNameLen * 2] = {0};
	m_effect->dispatcher(m_effect, effGetProgramName, 0, 0, prog, 0.0f);
	if (ibox.DoModal(L"New Preset Name",
		L"\
Please enter the preset name. \
Keep in mind that VST program names \
are limited in length to 24 characters.",
		stringcvt::string_wide_from_ansi(prog)) == IDOK )
	{
		string8 name;
		const int len = wcslen(ibox.Text);
		for (int i = 0; i < len; i++)
		{
			wchar_t c = ibox.Text[i];
			if (!is_path_bad_char(c) && c != L'\\' && c != L'/')
			{
				name.add_char(c);
			}
		}
		if (name.get_length() < 1) name = "default";
		//name.(ibox.Text);
		string8 fname = m_dir;
		fname << name << ".fxp";
		m_vst->store_fxp(fname, &name);
		m_vst->load_fxp(fname);
		load_presets();
	}
}

void editor::on_delete(UINT, int, ATL::CWindow)
{
	string_os_from_utf8 thedir(m_dir);
	for (int i = 0; i < m_presets->GetCount(); i++)
	{
		wchar_t txt[MAX_PATH] = {0};
		m_presets->GetText(i, txt);
		if (txt[0] != L'*' && m_presets->GetSel(i) > 0) // Built-in preset
		{
			wchar_t* fname = new wchar_t[m_dir.length() + MAX_PATH];
			wsprintf(fname, L"%s%s.fxp", thedir.get_ptr(), txt);
			DeleteFile(fname);
			delete fname;
		}
	}
}