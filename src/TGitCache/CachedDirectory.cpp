// TortoiseSVN - a Windows shell extension for easy version control

// External Cache Copyright (C) 2005-2008 - TortoiseSVN

// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//
#include "StdAfx.h"
#include ".\cacheddirectory.h"
//#include "SVNHelpers.h"
#include "GitStatusCache.h"
#include "GitStatus.h"
#include <set>

CCachedDirectory::CCachedDirectory(void)
{
	m_currentStatusFetchingPathTicks = 0;
	m_bCurrentFullStatusValid = false;
	m_currentFullStatus = m_mostImportantFileStatus = git_wc_status_none;
	m_bRecursive = true;
	m_indexFileTime = 0;
	m_FullStatusFetched = false;
}

CCachedDirectory::~CCachedDirectory(void)
{
}

CCachedDirectory::CCachedDirectory(const CTGitPath& directoryPath)
{
	ATLASSERT(directoryPath.IsDirectory() || !PathFileExists(directoryPath.GetWinPath()));

	m_directoryPath = directoryPath;

	m_currentStatusFetchingPathTicks = 0;
	m_bCurrentFullStatusValid = false;
	m_currentFullStatus = m_mostImportantFileStatus = git_wc_status_none;
	m_bRecursive = true;
}

BOOL CCachedDirectory::SaveToDisk(FILE * pFile)
{
	AutoLocker lock(m_critSec);
#define WRITEVALUETOFILE(x) if (fwrite(&x, sizeof(x), 1, pFile)!=1) return false;

	unsigned int value = GIT_CACHE_VERSION;
	WRITEVALUETOFILE(value);	// 'version' of this save-format
	value = (int)m_entryCache.size();
	WRITEVALUETOFILE(value);	// size of the cache map
	// now iterate through the maps and save every entry.
	for (CacheEntryMap::iterator I = m_entryCache.begin(); I != m_entryCache.end(); ++I)
	{
		const CString& key = I->first;
		value = key.GetLength();
		WRITEVALUETOFILE(value);
		if (value)
		{
			if (fwrite((LPCTSTR)key, sizeof(TCHAR), value, pFile)!=value)
				return false;
			if (!I->second.SaveToDisk(pFile))
				return false;
		}
	}
	value = (int)m_childDirectories.size();
	WRITEVALUETOFILE(value);
	for (ChildDirStatus::iterator I = m_childDirectories.begin(); I != m_childDirectories.end(); ++I)
	{
		const CString& path = I->first.GetWinPathString();
		value = path.GetLength();
		WRITEVALUETOFILE(value);
		if (value)
		{
			if (fwrite((LPCTSTR)path, sizeof(TCHAR), value, pFile)!=value)
				return false;
			git_wc_status_kind status = I->second;
			WRITEVALUETOFILE(status);
		}
	}
	WRITEVALUETOFILE(m_indexFileTime);
	WRITEVALUETOFILE(m_Head.m_hash);
//	WRITEVALUETOFILE(m_propsFileTime);
	value = m_directoryPath.GetWinPathString().GetLength();
	WRITEVALUETOFILE(value);
	if (value)
	{
		if (fwrite(m_directoryPath.GetWinPath(), sizeof(TCHAR), value, pFile)!=value)
			return false;
	}
	if (!m_ownStatus.SaveToDisk(pFile))
		return false;
	WRITEVALUETOFILE(m_currentFullStatus);
	WRITEVALUETOFILE(m_mostImportantFileStatus);
	return true;
}

BOOL CCachedDirectory::LoadFromDisk(FILE * pFile)
{
	AutoLocker lock(m_critSec);
#define LOADVALUEFROMFILE(x) if (fread(&x, sizeof(x), 1, pFile)!=1) return false;
	try
	{
		unsigned int value = 0;
		LOADVALUEFROMFILE(value);
		if (value != GIT_CACHE_VERSION)
			return false;		// not the correct version
		int mapsize = 0;
		LOADVALUEFROMFILE(mapsize);
		for (int i=0; i<mapsize; ++i)
		{
			LOADVALUEFROMFILE(value);
			if (value > MAX_PATH)
				return false;
			if (value)
			{
				CString sKey;
				if (fread(sKey.GetBuffer(value+1), sizeof(TCHAR), value, pFile)!=value)
				{
					sKey.ReleaseBuffer(0);
					return false;
				}
				sKey.ReleaseBuffer(value);
				CStatusCacheEntry entry;
				if (!entry.LoadFromDisk(pFile))
					return false;
				m_entryCache[sKey] = entry;
			}
		}
		LOADVALUEFROMFILE(mapsize);
		for (int i=0; i<mapsize; ++i)
		{
			LOADVALUEFROMFILE(value);
			if (value > MAX_PATH)
				return false;
			if (value)
			{
				CString sPath;
				if (fread(sPath.GetBuffer(value), sizeof(TCHAR), value, pFile)!=value)
				{
					sPath.ReleaseBuffer(0);
					return false;
				}
				sPath.ReleaseBuffer(value);
				git_wc_status_kind status;
				LOADVALUEFROMFILE(status);
				m_childDirectories[CTGitPath(sPath)] = status;
			}
		}
		LOADVALUEFROMFILE(m_indexFileTime);
		LOADVALUEFROMFILE(m_Head.m_hash);
//		LOADVALUEFROMFILE(m_propsFileTime);
		LOADVALUEFROMFILE(value);
		if (value > MAX_PATH)
			return false;
		if (value)
		{
			CString sPath;
			if (fread(sPath.GetBuffer(value+1), sizeof(TCHAR), value, pFile)!=value)
			{
				sPath.ReleaseBuffer(0);
				return false;
			}
			sPath.ReleaseBuffer(value);
			m_directoryPath.SetFromWin(sPath);
		}
		if (!m_ownStatus.LoadFromDisk(pFile))
			return false;

		LOADVALUEFROMFILE(m_currentFullStatus);
		LOADVALUEFROMFILE(m_mostImportantFileStatus);
	}
	catch ( CAtlException )
	{
		return false;
	}
	return true;

}
/// bFetch is true, fetch all status, call by crawl.  
/// bFetch is false, get cache status, return quickly.

CStatusCacheEntry CCachedDirectory::GetStatusForMember(const CTGitPath& path, bool bRecursive,  bool bFetch /* = true */)
{
	CString strCacheKey;
	bool bThisDirectoryIsUnversioned = false;
	bool bRequestForSelf = false;
	if(path.IsEquivalentToWithoutCase(m_directoryPath))
	{
		bRequestForSelf = true;
	}
//OutputDebugStringA("GetStatusForMember: ");OutputDebugStringW(path.GetWinPathString());OutputDebugStringA("\r\n");
	// In all most circumstances, we ask for the status of a member of this directory.
	ATLASSERT(m_directoryPath.IsEquivalentToWithoutCase(path.GetContainingDirectory()) || bRequestForSelf);

	CString sProjectRoot;
	const BOOL bIsVersionedPath = path.HasAdminDir(&sProjectRoot);

	ATLTRACE(_T("%d GetStatusForMember for %s\n"),bFetch, path.GetWinPath());

	//If is not version control path
	if( !bIsVersionedPath)
	{
		ATLTRACE(_T("%s is not underversion control\n"), path.GetWinPath());
		return CStatusCacheEntry();
	}
	
	// We've not got this item in the cache - let's add it
	// We never bother asking SVN for the status of just one file, always for its containing directory

	if (g_GitAdminDir.IsAdminDirPath(path.GetWinPathString()))
	{
		// We're being asked for the status of an .git directory
		// It's not worth asking for this
		return CStatusCacheEntry();
	}

	CString subpaths = path.GetGitPathString();
	if(subpaths.GetLength() >= sProjectRoot.GetLength())
	{		
		if(subpaths[sProjectRoot.GetLength()] == _T('/'))
			subpaths=subpaths.Right(subpaths.GetLength() - sProjectRoot.GetLength()-1);
		else
			subpaths=subpaths.Right(subpaths.GetLength() - sProjectRoot.GetLength());
	}
	
	
	GitStatus *pGitStatus = &CGitStatusCache::Instance().m_GitStatus;
	bool IsIndexHeadChanged = pGitStatus->IsGitReposChanged(sProjectRoot, subpaths, GIT_MODE_INDEX);

	CGitHash head;
	
	pGitStatus->GetHeadHash(sProjectRoot,head);

	bool isVersion =true;
	pGitStatus->IsUnderVersionControl(sProjectRoot, subpaths, path.IsDirectory(), &isVersion);

// Handle Untrack file and directory
//===================================
	if(!isVersion)
	{	
		/* if path is not under version control */
		/* Just need check if it is ignore, it is very quick */
		bool isIgnoreFileChanged=false;
		
		isIgnoreFileChanged = pGitStatus->IsGitReposChanged(sProjectRoot, subpaths, GIT_MODE_IGNORE);

		if( isIgnoreFileChanged)
		{
			pGitStatus->LoadIgnoreFile(sProjectRoot, subpaths);
		}

		std::vector<__int64> timelist;
		pGitStatus->GetIgnoreFileChangeTimeList((CString &)this->m_directoryPath.GetWinPathString(), timelist);
		
		if(timelist.size() != this->m_IgnoreFileTimeList.size())
			isIgnoreFileChanged =true;

		if(!isIgnoreFileChanged)
			for(int i=0;i<timelist.size();i++)
				if(timelist[i] != m_IgnoreFileTimeList[i])
				{
					isIgnoreFileChanged =true;
					break;
				}

		m_IgnoreFileTimeList = timelist;

		if(isIgnoreFileChanged)
			m_entryCache.clear();

		ATLTRACE(_T("Ignore Changed %d for %s\n"), isIgnoreFileChanged, path.GetWinPath());

		if(path.IsDirectory())
		{

			CCachedDirectory * dirEntry = CGitStatusCache::Instance().GetDirectoryCacheEntry(path,
											false); /* we needn't watch untracked directory*/

			if(dirEntry)
			{
				AutoLocker lock(dirEntry->m_critSec);

				git_wc_status_kind dirstatus = dirEntry->GetCurrentFullStatus() ;
				if( dirstatus == git_wc_status_none || dirstatus >= git_wc_status_normal || isIgnoreFileChanged )
				{/* status have not initialized*/
					git_wc_status2_t status2;
					bool isignore = false;
					pGitStatus->IsIgnore(sProjectRoot,subpaths,&isignore);
					status2.text_status = status2.prop_status = 
						(isignore? git_wc_status_ignored:git_wc_status_unversioned);

					dirEntry->m_ownStatus.SetStatus(&status2);

				}
				return dirEntry->m_ownStatus;
			}

		}else /* path is file */
		{
			AutoLocker lock(m_critSec);
			strCacheKey = GetCacheKey(path);

			CacheEntryMap::iterator itMap = m_entryCache.find(strCacheKey);
			if(itMap == m_entryCache.end() || isIgnoreFileChanged)
			{
				git_wc_status2_t status2;
				bool isignore = false;
				pGitStatus->IsIgnore(sProjectRoot,subpaths,&isignore);
				status2.text_status = status2.prop_status = 
					(isignore? git_wc_status_ignored:git_wc_status_unversioned);
				AddEntry(path, &status2);
				return m_entryCache[strCacheKey];
			}else
			{
				return itMap->second;
			}
		}
		return CStatusCacheEntry();

	}

/// Handle track file and directory
//==================================
	/* Get Status from git */
	if( (head == m_Head) && (m_indexFileTime==pGitStatus->GetIndexFileTime(sProjectRoot)))
	{	
		if(bFetch && !m_FullStatusFetched)
		{
			/* If crawl, fetch new updated status any way*/
			EnumFiles();
			m_FullStatusFetched = true;
		}

		if(path.IsDirectory())
		{
			// We don't have directory status in our cache
			// Ask the directory if it knows its own status
			
			CCachedDirectory * dirEntry = CGitStatusCache::Instance().GetDirectoryCacheEntry(path);
			if( dirEntry)
			{
				if (dirEntry->IsOwnStatusValid())
					return dirEntry->GetOwnStatus(bRecursive);
				else
				{
					/* cache have outof date, need crawl again*/
					AutoLocker lock(dirEntry->m_critSec);
					ChildDirStatus::const_iterator it;
					for(it = dirEntry->m_childDirectories.begin(); it != dirEntry->m_childDirectories.end(); ++it)
					{
						CGitStatusCache::Instance().AddFolderForCrawling(it->first);
					}
				}
			
			}
			if(bFetch)
			{
				ATLTRACE(_T("Clear Dir %s mostImportantFileStatus\n"), path.GetWinPath());
				AutoLocker lock(m_critSec);
				m_mostImportantFileStatus = git_wc_status_normal; /* unverstion file have handle at begin */
				m_childDirectories.clear();
				m_entryCache.clear();
				m_ownStatus.SetStatus(NULL);
				m_bRecursive = bRecursive;

				EnumFiles();
				m_FullStatusFetched = true;
			}else
			{
				CGitStatusCache::Instance().AddFolderForCrawling(path);
			}
			return CStatusCacheEntry(this->m_ownStatus);
		}
		else
		{
			// Get File status;
			// because all untrack file have handle at first, only tracked file status got to here
			git_wc_status_kind status = git_wc_status_normal;
			{
				// if we currently are fetching the status of the directory
				// we want the status for, we just return an empty entry here
				// and don't wait for that fetching to finish.
				// That's because fetching the status can take a *really* long
				// time (e.g. if a commit is also in progress on that same
				// directory), and we don't want to make the explorer appear
				// to hang.
				AutoLocker pathlock(m_critSecPath);
				if ((!bFetch)&&(!m_currentStatusFetchingPath.IsEmpty()))
				{
					if ((m_currentStatusFetchingPath.IsAncestorOf(path))&&((m_currentStatusFetchingPathTicks + 1000)<GetTickCount()))
					{
						ATLTRACE(_T("returning empty status (status fetch in progress) for %s\n"), path.GetWinPath());
						m_currentFullStatus = m_mostImportantFileStatus = git_wc_status_normal;
						return CStatusCacheEntry(git_wc_status_normal);
					}
				}
			}
			// Look up a file in our own cache
			AutoLocker lock(m_critSec);
			strCacheKey = GetCacheKey(path);
			CacheEntryMap::iterator itMap = m_entryCache.find(strCacheKey);
			if(itMap != m_entryCache.end())
			{
				// We've hit the cache - check for timeout
				if(!itMap->second.HasExpired((long)GetTickCount()))
				{
					if(itMap->second.DoesFileTimeMatch(path.GetLastWriteTime()))
					{
						if ((itMap->second.GetEffectiveStatus()!=git_wc_status_missing)||(!PathFileExists(path.GetWinPath())))
						{
							// Note: the filetime matches after a modified has been committed too.
							// So in that case, we would return a wrong status (e.g. 'modified' instead
							// of 'normal') here.
							return itMap->second;
						}
					}
				}
			}

			if(bFetch)
			{
				//update single full file status
				ATLTRACE(_T("Fetch file status %s\n"), path);
				pGitStatus->GetFileStatus(sProjectRoot, subpaths, &status, true, false,true, GetStatusCallback,this);
			}else
			{
				/* get status very quickly
				   Full status may load Head Tree.
				*/ 
				pGitStatus->GetFileStatus(sProjectRoot, subpaths, &status, false, false,true, NULL,NULL);
				CGitStatusCache::Instance().AddFolderForCrawling(path.GetContainingDirectory());
				m_FullStatusFetched = false;
			}			
			return CStatusCacheEntry(status);
		}
	}
	else /* Index or Head file changed*/
	{
		/* Clear all cache status */

		AutoLocker pathlock(m_critSecPath);
		if ((!bFetch)&&(!m_currentStatusFetchingPath.IsEmpty()))
		{
			if ((m_currentStatusFetchingPath.IsAncestorOf(path))&&((m_currentStatusFetchingPathTicks + 1000)<GetTickCount()))
			{
				ATLTRACE(_T("returning empty status (status fetch in progress) for %s\n"), path.GetWinPath());
				m_currentFullStatus = m_mostImportantFileStatus = git_wc_status_normal;
				return CStatusCacheEntry(git_wc_status_normal);
			}
		}

		// if we're fetching the status for the explorer,
		// we don't refresh the status but use the one
		// we already have (to save time and make the explorer
		// more responsive in stress conditions).
		// We leave the refreshing to the crawler.
		if ((!bFetch))
		{
			CGitStatusCache::Instance().AddFolderForCrawling(path.GetDirectory());
			
		}
		
		m_indexFileTime = pGitStatus->GetIndexFileTime(sProjectRoot);
		m_Head = head;
		m_FullStatusFetched  = false;

		AutoLocker lock(m_critSec);
//		m_indexFileTime = indexFilePath.GetLastWriteTime();
//		m_propsFileTime = propsDirPath.GetLastWriteTime();
		m_entryCache.clear();
		strCacheKey = GetCacheKey(path);

		git_wc_status2_t status2;
		status2.prop_status = status2.text_status = git_wc_status_normal;
		this->m_mostImportantFileStatus = this->m_currentFullStatus = git_wc_status_normal;
		m_ownStatus.SetStatus(&status2);
		
		
	}

//  Cache status have been invalidate, head or index changed.
// =====================================================
	{
		{
			AutoLocker pathlock(m_critSecPath);
			if ((!bFetch)&&(!m_currentStatusFetchingPath.IsEmpty()))
			{
				if ((m_currentStatusFetchingPath.IsAncestorOf(path))&&((m_currentStatusFetchingPathTicks + 1000)<GetTickCount()))
				{
					ATLTRACE(_T("returning empty status (status fetch in progress) for %s\n"), path.GetWinPath());
					m_currentFullStatus = m_mostImportantFileStatus = git_wc_status_normal;
					return CStatusCacheEntry(git_wc_status_normal);
				}
			}
		}

		
		/* If bFetch false, means get basic status as soon as possible.
			   If bFetch false, means get full status from crawl thread
			   when bfetch false, just fetch status according to index file.
			   can't fetch HEAD tree. fetch HEAD tree will take some long time
		*/
		BOOL pErr = EnumFiles(NULL, bFetch);

		{
			AutoLocker pathlock(m_critSecPath);
			m_currentStatusFetchingPath.Reset();
		}

		ATLTRACE(_T("git_enum_files finished for '%s'\n"), m_directoryPath.GetWinPath(), path.GetWinPath());

	}
	// Now that we've refreshed our SVN status, we can see if it's 
	// changed the 'most important' status value for this directory.
	// If it has, then we should tell our parent
	if(bFetch)
		UpdateCurrentStatus();

	if (path.IsDirectory())
	{
		CCachedDirectory * dirEntry = CGitStatusCache::Instance().GetDirectoryCacheEntry(path);
		if ((dirEntry)&&(dirEntry->IsOwnStatusValid()))
		{
			/// status should be valid, needn't add to crawl.
		//	CGitStatusCache::Instance().AddFolderForCrawling(path);
			return dirEntry->GetOwnStatus(bRecursive);
		}

		// If the status *still* isn't valid here, it means that 
		// the current directory is unversioned, and we shall need to ask its children for info about themselves
		//if (dirEntry)
		//	return dirEntry->GetStatusForMember(path,bRecursive);

		CGitStatusCache::Instance().AddFolderForCrawling(path);
		return CStatusCacheEntry(git_wc_status_normal);;
	}
	else
	{
		CacheEntryMap::iterator itMap = m_entryCache.find(strCacheKey);
		if(itMap != m_entryCache.end())
		{
			return itMap->second;
		}
	}

	return CStatusCacheEntry(git_wc_status_normal);
}

int CCachedDirectory::EnumFiles(CTGitPath *path , bool IsFull)
{
	CString sProjectRoot;
	if(path)
		path->HasAdminDir(&sProjectRoot);
	else
		m_directoryPath.HasAdminDir(&sProjectRoot);	

	ATLASSERT( !m_directoryPath.IsEmpty() );

	LPCTSTR lpszSubPath = NULL;
	CString sSubPath;
	
	CString s;
	if(path)
		s=path->GetWinPath();
	else
		s=m_directoryPath.GetDirectory().GetWinPathString();

	if (s.GetLength() > sProjectRoot.GetLength())
	{
		// skip initial slash if necessary
		if(s[sProjectRoot.GetLength()] == _T('\\'))
			sSubPath = s.Right(s.GetLength() - sProjectRoot.GetLength() -1);
		else 
			sSubPath = s.Right(s.GetLength() - sProjectRoot.GetLength() );
	}
	
	GitStatus *pStatus = &CGitStatusCache::Instance().m_GitStatus;
	git_wc_status_kind status;

	if(path)
		pStatus->GetFileStatus(sProjectRoot, sSubPath, &status, IsFull, false,true, GetStatusCallback,this);
	else
		pStatus->EnumDirStatus(sProjectRoot, sSubPath, &status, IsFull, false, true, GetStatusCallback,this);

	m_mostImportantFileStatus = GitStatus::GetMoreImportant(m_mostImportantFileStatus, status);

	return 0;
}
void 
CCachedDirectory::AddEntry(const CTGitPath& path, const git_wc_status2_t* pGitStatus, DWORD validuntil /* = 0*/)
{
	AutoLocker lock(m_critSec);
	if(path.IsDirectory())
	{
		CCachedDirectory * childDir = CGitStatusCache::Instance().GetDirectoryCacheEntry(path);
		if (childDir)
		{
			if ((childDir->GetCurrentFullStatus() != git_wc_status_missing)||(pGitStatus==NULL)||(pGitStatus->text_status != git_wc_status_unversioned))
			{
				if(pGitStatus)
				{
					if(childDir->GetCurrentFullStatus() != GitStatus::GetMoreImportant(pGitStatus->prop_status, pGitStatus->text_status))
					{
						CGitStatusCache::Instance().UpdateShell(path);
						ATLTRACE(_T("shell update for %s\n"), path.GetWinPath());
					}
				}
				childDir->m_ownStatus.SetStatus(pGitStatus);
			}
			childDir->m_ownStatus.SetKind(git_node_dir);

			
		}
	}
	else
	{
		if(!(this->m_directoryPath == path.GetContainingDirectory()))
		{
			ATLTRACE(_T("parent path miss\n"));
		}
		CString cachekey = GetCacheKey(path);
		CacheEntryMap::iterator entry_it = m_entryCache.lower_bound(cachekey);
		if (entry_it != m_entryCache.end() && entry_it->first == cachekey)
		{
			if (pGitStatus)
			{
				if (entry_it->second.GetEffectiveStatus() > git_wc_status_none &&
					entry_it->second.GetEffectiveStatus() != GitStatus::GetMoreImportant(pGitStatus->prop_status, pGitStatus->text_status))
				{
					CGitStatusCache::Instance().UpdateShell(path);
					ATLTRACE(_T("shell update for %s\n"), path.GetWinPath());
				}
			}
		}
		else
		{
			entry_it = m_entryCache.insert(entry_it, std::make_pair(cachekey, CStatusCacheEntry()));
		}
		entry_it->second = CStatusCacheEntry(pGitStatus, path.GetLastWriteTime(), path.IsReadOnly(), validuntil);
		// TEMP(?): git status doesn't not have "entry" that contains node type, so manually set as file
		entry_it->second.SetKind(git_node_file);

		ATLTRACE(_T("Path Entry Add %s %s %s %d\n"), path.GetWinPath(), cachekey, m_directoryPath.GetWinPath(), pGitStatus->text_status);
	}
}


CString 
CCachedDirectory::GetCacheKey(const CTGitPath& path)
{
	// All we put into the cache as a key is just the end portion of the pathname
	// There's no point storing the path of the containing directory for every item
	return path.GetWinPathString().Mid(m_directoryPath.GetWinPathString().GetLength());
}

CString 
CCachedDirectory::GetFullPathString(const CString& cacheKey)
{
	return m_directoryPath.GetWinPathString() + _T("\\") + cacheKey;
}

BOOL CCachedDirectory::GetStatusCallback(CString & path, git_wc_status_kind status,bool isDir, void *pUserData)
{
	CCachedDirectory* pThis = (CCachedDirectory*)pUserData;

	git_wc_status2_t _status;
	git_wc_status2_t *status2 = &_status;

	status2->prop_status = status2->text_status = status;

	CTGitPath gitPath;


//	if(status->entry)
	{
		//if ((status->text_status != git_wc_status_none)&&(status->text_status != git_wc_status_missing))
		gitPath.SetFromUnknown(path);
		//status.SetFromGit(path, pFile->nFlags & WGFF_Directory);
		/*else
			svnPath.SetFromGit(path);*/

		if (isDir)
		{	/*gitpath is directory*/
			if ( !gitPath.IsEquivalentToWithoutCase(pThis->m_directoryPath) )
			{
				if (!gitPath.Exists())
				{
					ATLTRACE(_T("Miss dir %s \n"), gitPath.GetWinPath());
					pThis->m_mostImportantFileStatus = GitStatus::GetMoreImportant(pThis->m_mostImportantFileStatus, git_wc_status_deleted);
				}

				if (pThis->m_bRecursive)
				{
					// Add any versioned directory, which is not our 'self' entry, to the list for having its status updated
//OutputDebugStringA("AddFolderCrawl: ");OutputDebugStringW(svnPath.GetWinPathString());OutputDebugStringA("\r\n");
					if(status >= git_wc_status_normal)
						if(status != git_wc_status_missing)
							if(status != git_wc_status_deleted)
								CGitStatusCache::Instance().AddFolderForCrawling(gitPath);
				}

				// Make sure we know about this child directory
				// This initial status value is likely to be overwritten from below at some point
				git_wc_status_kind s = GitStatus::GetMoreImportant(status2->text_status, status2->prop_status);
				CCachedDirectory * cdir = CGitStatusCache::Instance().GetDirectoryCacheEntryNoCreate(gitPath);
				if (cdir)
				{
					// This child directory is already in our cache!
					// So ask this dir about its recursive status
					git_wc_status_kind st = GitStatus::GetMoreImportant(s, cdir->GetCurrentFullStatus());
					AutoLocker lock(pThis->m_critSec);
					pThis->m_childDirectories[gitPath] = st;
					ATLTRACE(_T("call 1 Update dir %s %d\n"), gitPath.GetWinPath(), st);
				}
				else
				{
					AutoLocker lock(pThis->m_critSec);
					// the child directory is not in the cache. Create a new entry for it in the cache which is
					// initially 'unversioned'. But we added that directory to the crawling list above, which
					// means the cache will be updated soon.
					CGitStatusCache::Instance().GetDirectoryCacheEntry(gitPath);
				
					pThis->m_childDirectories[gitPath] = s;
					ATLTRACE(_T("call 2 Update dir %s %d\n"), gitPath.GetWinPath(), s);
				}
			}
		}
		else /* gitpath is file*/
		{
			// Keep track of the most important status of all the files in this directory
			// Don't include subdirectories in this figure, because they need to provide their 
			// own 'most important' value
			pThis->m_mostImportantFileStatus = GitStatus::GetMoreImportant(pThis->m_mostImportantFileStatus, status2->text_status);
			pThis->m_mostImportantFileStatus = GitStatus::GetMoreImportant(pThis->m_mostImportantFileStatus, status2->prop_status);
			if (((status2->text_status == git_wc_status_unversioned)||(status2->text_status == git_wc_status_none))
				&&(CGitStatusCache::Instance().IsUnversionedAsModified()))
			{
				// treat unversioned files as modified
				if (pThis->m_mostImportantFileStatus != git_wc_status_added)
					pThis->m_mostImportantFileStatus = GitStatus::GetMoreImportant(pThis->m_mostImportantFileStatus, git_wc_status_modified);
			}
		}
	}

	pThis->AddEntry(gitPath, status2);

	return FALSE;
}

#if 0
git_error_t * CCachedDirectory::GetStatusCallback(void *baton, const char *path, git_wc_status2_t *status)
{
	CCachedDirectory* pThis = (CCachedDirectory*)baton;

	if (path == NULL)
		return 0;
		
	CTGitPath svnPath;

	if(status->entry)
	{
		if ((status->text_status != git_wc_status_none)&&(status->text_status != git_wc_status_missing))
			svnPath.SetFromSVN(path, (status->entry->kind == svn_node_dir));
		else
			svnPath.SetFromSVN(path);

		if(svnPath.IsDirectory())
		{
			if(!svnPath.IsEquivalentToWithoutCase(pThis->m_directoryPath))
			{
				if (pThis->m_bRecursive)
				{
					// Add any versioned directory, which is not our 'self' entry, to the list for having its status updated
					CGitStatusCache::Instance().AddFolderForCrawling(svnPath);
				}

				// Make sure we know about this child directory
				// This initial status value is likely to be overwritten from below at some point
				git_wc_status_kind s = GitStatus::GetMoreImportant(status->text_status, status->prop_status);
				CCachedDirectory * cdir = CGitStatusCache::Instance().GetDirectoryCacheEntryNoCreate(svnPath);
				if (cdir)
				{
					// This child directory is already in our cache!
					// So ask this dir about its recursive status
					git_wc_status_kind st = GitStatus::GetMoreImportant(s, cdir->GetCurrentFullStatus());
					AutoLocker lock(pThis->m_critSec);
					pThis->m_childDirectories[svnPath] = st;
				}
				else
				{
					// the child directory is not in the cache. Create a new entry for it in the cache which is
					// initially 'unversioned'. But we added that directory to the crawling list above, which
					// means the cache will be updated soon.
					CGitStatusCache::Instance().GetDirectoryCacheEntry(svnPath);
					AutoLocker lock(pThis->m_critSec);
					pThis->m_childDirectories[svnPath] = s;
				}
			}
		}
		else
		{
			// Keep track of the most important status of all the files in this directory
			// Don't include subdirectories in this figure, because they need to provide their 
			// own 'most important' value
			pThis->m_mostImportantFileStatus = GitStatus::GetMoreImportant(pThis->m_mostImportantFileStatus, status->text_status);
			pThis->m_mostImportantFileStatus = GitStatus::GetMoreImportant(pThis->m_mostImportantFileStatus, status->prop_status);
			if (((status->text_status == git_wc_status_unversioned)||(status->text_status == git_wc_status_none))
				&&(CGitStatusCache::Instance().IsUnversionedAsModified()))
			{
				// treat unversioned files as modified
				if (pThis->m_mostImportantFileStatus != git_wc_status_added)
					pThis->m_mostImportantFileStatus = GitStatus::GetMoreImportant(pThis->m_mostImportantFileStatus, git_wc_status_modified);
			}
		}
	}
	else
	{
		svnPath.SetFromSVN(path);
		// Subversion returns no 'entry' field for versioned folders if they're
		// part of another working copy (nested layouts).
		// So we have to make sure that such an 'unversioned' folder really
		// is unversioned.
		if (((status->text_status == git_wc_status_unversioned)||(status->text_status == git_wc_status_missing))&&(!svnPath.IsEquivalentToWithoutCase(pThis->m_directoryPath))&&(svnPath.IsDirectory()))
		{
			if (svnPath.HasAdminDir())
			{
				CGitStatusCache::Instance().AddFolderForCrawling(svnPath);
				// Mark the directory as 'versioned' (status 'normal' for now).
				// This initial value will be overwritten from below some time later
				{
					AutoLocker lock(pThis->m_critSec);
					pThis->m_childDirectories[svnPath] = git_wc_status_normal;
				}
				// Make sure the entry is also in the cache
				CGitStatusCache::Instance().GetDirectoryCacheEntry(svnPath);
				// also mark the status in the status object as normal
				status->text_status = git_wc_status_normal;
			}
		}
		else if (status->text_status == git_wc_status_external)
		{
			CGitStatusCache::Instance().AddFolderForCrawling(svnPath);
			// Mark the directory as 'versioned' (status 'normal' for now).
			// This initial value will be overwritten from below some time later
			{
				AutoLocker lock(pThis->m_critSec);
				pThis->m_childDirectories[svnPath] = git_wc_status_normal;
			}
			// we have added a directory to the child-directory list of this
			// directory. We now must make sure that this directory also has
			// an entry in the cache.
			CGitStatusCache::Instance().GetDirectoryCacheEntry(svnPath);
			// also mark the status in the status object as normal
			status->text_status = git_wc_status_normal;
		}
		else
		{
			if (svnPath.IsDirectory())
			{
				AutoLocker lock(pThis->m_critSec);
				pThis->m_childDirectories[svnPath] = GitStatus::GetMoreImportant(status->text_status, status->prop_status);
			}
			else if ((CGitStatusCache::Instance().IsUnversionedAsModified())&&(status->text_status != git_wc_status_missing))
			{
				// make this unversioned item change the most important status of this
				// folder to modified if it doesn't already have another status
				if (pThis->m_mostImportantFileStatus != git_wc_status_added)
					pThis->m_mostImportantFileStatus = GitStatus::GetMoreImportant(pThis->m_mostImportantFileStatus, git_wc_status_modified);
			}
		}
	}

	pThis->AddEntry(svnPath, status);

	return 0;
}
#endif

bool 
CCachedDirectory::IsOwnStatusValid() const
{
	return m_ownStatus.HasBeenSet() && 
		   !m_ownStatus.HasExpired(GetTickCount()) &&
		   // 'external' isn't a valid status. That just
		   // means the folder is not part of the current working
		   // copy but it still has its own 'real' status
		   m_ownStatus.GetEffectiveStatus()!=git_wc_status_external &&
		   m_ownStatus.IsKindKnown();
}

void CCachedDirectory::Invalidate()
{
	m_ownStatus.Invalidate();
}

git_wc_status_kind CCachedDirectory::CalculateRecursiveStatus()
{
	// Combine our OWN folder status with the most important of our *FILES'* status.
	git_wc_status_kind retVal = GitStatus::GetMoreImportant(m_mostImportantFileStatus, m_ownStatus.GetEffectiveStatus());

	// NOTE: TSVN marks dir as modified if it contains added/deleted/missing files, but we prefer the most important
	//       status to propagate upward in its original state
	/*if ((retVal != git_wc_status_modified)&&(retVal != m_ownStatus.GetEffectiveStatus()))
	{
		if ((retVal == git_wc_status_added)||(retVal == git_wc_status_deleted)||(retVal == git_wc_status_missing))
			retVal = git_wc_status_modified;
	}*/

	// Now combine all our child-directorie's status
	
	AutoLocker lock(m_critSec);
	ChildDirStatus::const_iterator it;
	for(it = m_childDirectories.begin(); it != m_childDirectories.end(); ++it)
	{
		retVal = GitStatus::GetMoreImportant(retVal, it->second);
		/*if ((retVal != git_wc_status_modified)&&(retVal != m_ownStatus.GetEffectiveStatus()))
		{
			if ((retVal == git_wc_status_added)||(retVal == git_wc_status_deleted)||(retVal == git_wc_status_missing))
				retVal = git_wc_status_modified;
		}*/
	}
	
	return retVal;
}

// Update our composite status and deal with things if it's changed
void CCachedDirectory::UpdateCurrentStatus()
{
	git_wc_status_kind newStatus = CalculateRecursiveStatus();
	ATLTRACE(_T("UpdateCurrentStatus %s new:%d old: %d\n"),
		m_directoryPath.GetWinPath(), 
		newStatus, m_currentFullStatus);
	if ((newStatus != m_currentFullStatus)&&(m_ownStatus.IsVersioned()))
	{
		if ((m_currentFullStatus != git_wc_status_none)&&(m_ownStatus.GetEffectiveStatus() != git_wc_status_missing))
		{
			// Our status has changed - tell the shell
			ATLTRACE(_T("Dir %s, status change from %d to %d, send shell notification\n"), m_directoryPath.GetWinPath(), m_currentFullStatus, newStatus);		
			CGitStatusCache::Instance().UpdateShell(m_directoryPath);
		}
		if (m_ownStatus.GetEffectiveStatus() != git_wc_status_missing)
			m_currentFullStatus = newStatus;
		else
			m_currentFullStatus = git_wc_status_missing;
	}
	// And tell our parent, if we've got one...
	// we tell our parent *always* about our status, even if it hasn't
	// changed. This is to make sure that the parent has really our current
	// status - the parent can decide itself if our status has changed
	// or not.
	CTGitPath parentPath = m_directoryPath.GetContainingDirectory();
	if(!parentPath.IsEmpty())
	{
		// We have a parent
		// just version controled directory need to cache.
		if(parentPath.HasAdminDir())
		{
			CCachedDirectory * cachedDir = CGitStatusCache::Instance().GetDirectoryCacheEntry(parentPath);
			if (cachedDir)
				cachedDir->UpdateChildDirectoryStatus(m_directoryPath, m_currentFullStatus);
		}
	}
}


// Receive a notification from a child that its status has changed
void CCachedDirectory::UpdateChildDirectoryStatus(const CTGitPath& childDir, git_wc_status_kind childStatus)
{
	git_wc_status_kind currentStatus = git_wc_status_none;
	{
		AutoLocker lock(m_critSec);
		currentStatus = m_childDirectories[childDir];
	}
	if ((currentStatus != childStatus)||(!IsOwnStatusValid()))
	{
		{
			AutoLocker lock(m_critSec);
			m_childDirectories[childDir] = childStatus;
		}
		UpdateCurrentStatus();
	}
}

CStatusCacheEntry CCachedDirectory::GetOwnStatus(bool bRecursive)
{
	// Don't return recursive status if we're unversioned ourselves.
	if(bRecursive && m_ownStatus.GetEffectiveStatus() > git_wc_status_unversioned)
	{
		CStatusCacheEntry recursiveStatus(m_ownStatus);
		UpdateCurrentStatus();
		recursiveStatus.ForceStatus(m_currentFullStatus);
		return recursiveStatus;				
	}
	else
	{
		return m_ownStatus;
	}
}

void CCachedDirectory::RefreshStatus(bool bRecursive)
{
	// Make sure that our own status is up-to-date
	GetStatusForMember(m_directoryPath,bRecursive);

	AutoLocker lock(m_critSec);
	// We also need to check if all our file members have the right date on them
	CacheEntryMap::iterator itMembers;
	std::set<CTGitPath> refreshedpaths;
	DWORD now = GetTickCount();
	if (m_entryCache.size() == 0)
		return;
	for (itMembers = m_entryCache.begin(); itMembers != m_entryCache.end(); ++itMembers)
	{
		if (itMembers->first)
		{
			CTGitPath filePath(m_directoryPath);
			filePath.AppendPathString(itMembers->first);
			std::set<CTGitPath>::iterator refr_it;
			if ((!filePath.IsEquivalentToWithoutCase(m_directoryPath))&&
				(((refr_it = refreshedpaths.lower_bound(filePath)) == refreshedpaths.end()) || !filePath.IsEquivalentToWithoutCase(*refr_it)))
			{
				if ((itMembers->second.HasExpired(now))||(!itMembers->second.DoesFileTimeMatch(filePath.GetLastWriteTime())))
				{
					lock.Unlock();
					// We need to request this item as well
					GetStatusForMember(filePath,bRecursive);
					// GetStatusForMember now has recreated the m_entryCache map.
					// So start the loop again, but add this path to the refreshed paths set
					// to make sure we don't refresh this path again. This is to make sure
					// that we don't end up in an endless loop.
					lock.Lock();
					refreshedpaths.insert(refr_it, filePath);
					itMembers = m_entryCache.begin();
					if (m_entryCache.size()==0)
						return;
					continue;
				}
				else if ((bRecursive)&&(itMembers->second.IsDirectory()))
				{
					// crawl all sub folders too! Otherwise a change deep inside the
					// tree which has changed won't get propagated up the tree.
					CGitStatusCache::Instance().AddFolderForCrawling(filePath);
				}
			}
		}
	}
}

void CCachedDirectory::RefreshMostImportant()
{
	CacheEntryMap::iterator itMembers;
	git_wc_status_kind newStatus = m_ownStatus.GetEffectiveStatus();
	for (itMembers = m_entryCache.begin(); itMembers != m_entryCache.end(); ++itMembers)
	{
		newStatus = GitStatus::GetMoreImportant(newStatus, itMembers->second.GetEffectiveStatus());
		if (((itMembers->second.GetEffectiveStatus() == git_wc_status_unversioned)||(itMembers->second.GetEffectiveStatus() == git_wc_status_none))
			&&(CGitStatusCache::Instance().IsUnversionedAsModified()))
		{
			// treat unversioned files as modified
			if (newStatus != git_wc_status_added)
				newStatus = GitStatus::GetMoreImportant(newStatus, git_wc_status_modified);
		}
	}
	if (newStatus != m_mostImportantFileStatus)
	{
		ATLTRACE(_T("status change of path %s\n"), m_directoryPath.GetWinPath());
		CGitStatusCache::Instance().UpdateShell(m_directoryPath);
	}
	m_mostImportantFileStatus = newStatus;
}
