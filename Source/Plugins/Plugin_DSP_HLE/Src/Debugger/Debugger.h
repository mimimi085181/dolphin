//////////////////////////////////////////////////////////////////////////////////////////
//
// Licensetype: GNU General Public License (GPL)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.
//
// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/
//
// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/
//
//////////////////////////////////////////////////////////////////////////////////////////

#ifndef __CDebugger_h__
#define __CDebugger_h__

// general things
#include <iostream>
#include <vector>

// wx stuff, I'm not sure if we use all these
#ifndef WX_PRECOMP
	#include <wx/wx.h>
	#include <wx/dialog.h>
#else
	#include <wx/wxprec.h>
#endif

#include <wx/button.h>
#include <wx/stattext.h>
#include <wx/statbox.h>
#include <wx/statbmp.h>
#include <wx/datetime.h> // for the timestamps

#include <wx/sizer.h>
#include <wx/notebook.h>
#include <wx/filepicker.h>
#include <wx/listctrl.h>
#include <wx/imaglist.h>

#include "../Globals.h"

class CPBView;
class IniFile;

// Window settings
#undef CDebugger_STYLE
#define CDebugger_STYLE wxDEFAULT_FRAME_STYLE | wxCLIP_CHILDREN | wxNO_FULL_REPAINT_ON_RESIZE

class CDebugger : public wxDialog
{
	private:
		DECLARE_EVENT_TABLE();
		
	public:
		CDebugger(wxWindow *parent, wxWindowID id = 1, const wxString &title = _("Sound Debugger"),
			const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize,
			long style = CDebugger_STYLE);

		virtual ~CDebugger();

		void Save(IniFile& _IniFile) const;
		void Load(IniFile& _IniFile);
	
		void NotifyUpdate();
		void OnUpdate(wxCommandEvent& event);

		void SaveFile(wxCommandEvent& event); // options
		void ShowHideConsole(wxCommandEvent& event);
		void DoShowHideConsole();
		void OnlyLooping(wxCommandEvent& event);


		void ChangeFrequency(wxCommandEvent& event); // update frequency
		void DoChangeFrequency();
		void ChangePreset(wxCommandEvent& event);
		void DoChangePreset();

		void OnSettingsCheck(wxCommandEvent& event); // settings				

		// ============== Mail
		void DoUpdateMail();
		void UpdateMail(wxNotebookEvent& event);
		void ChangeMail(wxCommandEvent& event);	
		void ReadDir();	
		bool NoDuplicate(std::string FileName);
		void OnGameChange(wxCommandEvent& event);
		void MailSettings(wxCommandEvent& event);
		void Readfile(std::string FileName, bool GC);
		std::string Readfile_(std::string FileName);
		int CountFiles(std::string FileName);		

		CPBView* m_GPRListView;
		std::vector<std::string> sMail, sMailEnd, sFullMail;
		wxRadioBox *m_RadioBox[4];
		
		bool gSaveFile; // main options
		bool gOnlyLooping;
		int gUpdFreq;// main update freq.
		bool gPreset; // main presets		

		bool gSSBM;  // main settings
		bool gSSBMremedy1;
		bool gSSBMremedy2;
		bool gSequenced;
		bool gVolume;
		bool gReset;		

		bool ScanMails; // mail settings
		bool StoreMails;

	private:

		// declarations
		wxNotebook *m_Notebook; // notebook
		wxPanel *m_PageMain;
		wxPanel *m_PageMail;

		wxTextCtrl * m_log, * m_log1; // mail

		wxCheckBox *m_Check[9];
		wxRadioButton *m_Radio[5];
		wxCheckListBox * m_settings, * m_gc, * m_wii, * m_gcwiiset;
		wxPanel *m_Controller;

		std::vector<std::string> all_all_files, all_files, gc_files, wii_files;

		// WARNING: Make sure these are not also elsewhere, for example in resource.h.
		enum
		{
			IDC_CHECK0 = 2000,
			IDC_CHECK1,
			IDC_CHECK2,
			IDC_CHECK3,
			IDC_CHECK4,
			IDC_CHECKLIST1, IDC_CHECKLIST2, IDC_CHECKLIST3, IDC_CHECKLIST4,
			IDC_RADIO0, IDC_RADIO1, IDC_RADIO2, IDC_RADIO3,
			IDG_LABEL1, IDG_LABEL2,
			ID_UPD,
			ID_SELC,
			ID_PRESETS,
			ID_GPR,
			ID_NOTEBOOK, // notebook
			ID_PAGEMAIN,
			ID_PAGEMAIL,
			ID_LOG, // mails
			ID_LOG1,
			ID_DUMMY_VALUE_ //don't remove this value unless you have other enum values

		};

		void OnClose(wxCloseEvent& event);
		void CreateGUIControls();		
};

#endif
