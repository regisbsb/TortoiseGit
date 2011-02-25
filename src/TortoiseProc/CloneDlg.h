#pragma once

#include "StandAloneDlg.h"
#include "HistoryCombo.h"
#include "MenuButton.h"
#include "registry.h"
#include "tooltip.h"
// CCloneDlg dialog

class CCloneDlg : public CResizableStandAloneDialog
{
	DECLARE_DYNCREATE(CCloneDlg)

public:
	CCloneDlg(CWnd* pParent = NULL);   // standard constructor
	virtual ~CCloneDlg();

// Dialog Data
	enum { IDD = IDD_CLONE};

protected:
	// Overrides
	virtual void OnOK();
	virtual void OnCancel();

	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support
	virtual BOOL OnInitDialog();

	CString m_ModuleName;
	CString m_OldURL;

	DECLARE_MESSAGE_MAP()

	afx_msg void OnBnClickedCloneBrowseUrl();
	afx_msg void OnBnClickedCloneDirBrowse();

	CHistoryCombo	m_URLCombo;
	CHistoryCombo	m_PuttyKeyCombo;

public:
	CString	m_Directory;
	CString	m_strPuttyKeyFile;
	CString	m_URL;
	BOOL	m_bAutoloadPuttyKeyFile;
	BOOL	m_bSVN;
	BOOL	m_bSVNTrunk;
	BOOL	m_bSVNTags;
	BOOL	m_bSVNBranch;
	BOOL	m_bSVNFrom;
	BOOL	m_bDepth;
	BOOL	m_bSVNUserName;
	BOOL	m_bBare;

	int		m_nDepth;

	CString	m_strSVNTrunk;
	CString	m_strSVNTags;
	CString	m_strSVNBranchs;
	CString	m_strUserName;

	int		m_nSVNFrom;

protected:
	CMenuButton	m_BrowseUrl;
	CRegDWORD	m_regBrowseUrl;

	afx_msg void OnBnClickedPuttykeyfileBrowse();
	afx_msg void OnBnClickedPuttykeyAutoload();
	afx_msg void OnCbnEditchangeUrlcombo();

	afx_msg void OnBnClickedCheckBare();
	afx_msg void OnBnClickedCheckSvn();
	afx_msg void OnBnClickedCheckSvnTrunk();
	afx_msg void OnBnClickedCheckSvnTag();
	afx_msg void OnBnClickedCheckSvnBranch();
	afx_msg void OnBnClickedCheckSvnFrom();
	afx_msg void OnBnClickedCheckDepth();

	CToolTips	m_tooltips;
	virtual BOOL PreTranslateMessage(MSG* pMsg);
	afx_msg void OnBnClickedCheckUsername();
};
