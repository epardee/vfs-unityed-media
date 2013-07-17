/*
 * unityed_media.c, v 1.01 2013/07/07
 *
 * Samba VFS module supporting multiple AVID clients sharing media.
 *
 * Copyright (C) 2005  Philip de Nier <philipn@users.sourceforge.net>
 * Copyright (C) 2012  Andrew Klaassen <clawsoon@yahoo.com>
 * Copyright (C) 2013  Milos Lukacek
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */


/*
 * Unityed Media is a Samba VFS module that allows multiple AVID
 * clients to share media.
 *
 * Add this module to the vfs objects option in your Samba share
 * configuration.
 * eg.
 *
 *   [avid_win]
 *	path = /video
 *	vfs objects = unityed_media
 *	...
 *
 * It is recommended that you separate out Samba shares for Mac
 * and Windows clients, and add the following options to the shares
 * for Windows clients  (NOTE: replace @ with *):
 *
 *	veto files = /.DS_Store/._@/.Trash@/.Spotlight@/.hidden/.hotfiles@/.vol/
 *	delete veto files = yes
 *
 * This prevents hidden files from Mac clients interfering with Windows
 * clients. If you find any more problem hidden files then add them to
 * the list.
 *
 * Notes:
 * This module is designed to work with AVID editing applications that
 * look in the Avid MediaFiles or OMFI MediaFiles directory for media.
 * It is not designed to work as expected in all circumstances for
 * general use.
 */


#include "includes.h"
#include "system/filesys.h"
#include "smbd/smbd.h"
#include "../smbd/globals.h"
#include "auth.h"
#include "../lib/tsocket/tsocket.h"

#define UM_INFO_DEBUG 10
#define UM_ERR_DEBUG 0

static const char* UM_MODULE_NAME = "unityed_media";
static const char* AVID_MEDIAFILES_DIRNAME = "Avid MediaFiles";
static const size_t AVID_MEDIAFILES_DIRNAME_LEN = 15;
static const char* AVID_MXF_DIRNAME = "Avid MediaFiles/MXF";
static const size_t AVID_MXF_DIRNAME_LEN = 19;
static const char* OMFI_MEDIAFILES_DIRNAME = "OMFI MediaFiles";
static const size_t OMFI_MEDIAFILES_DIRNAME_LEN = 15;
static const char* APPLE_DOUBLE_PREFIX = "._";
static const size_t APPLE_DOUBLE_PREFIX_LEN = 2;
static int vfs_um_debug_level = DBGC_VFS;

/* supplements the directory list stream */
typedef struct um_dirinfo_struct
{
	DIR* dirstream;
	char *dirpath;
	char *clientPath;
	bool isInMediaFiles;
	char *clientSubDirname;
} um_dirinfo_struct;

/*returns first group of digits in path*/
static char *get_digit_group(const char* path)
{
	DEBUG(UM_INFO_DEBUG, ("get_digit_group entering with path '%s'\n", path));
	char *num_suffix = NULL;
	char *p = path;
	while (*p) {
    
    if (isdigit(*p)) {
        num_suffix = (char*)strtoul(p, &p, 10); 
        DEBUG(UM_INFO_DEBUG, ("num_suffix = '%d'\n", num_suffix));
        break;
    } else {
        p++;
    }
	}
	return num_suffix;
}

/* Add "_<remote_name>.<number>" suffix to path or filename.
 *
 * Success: return 0
 * Failure: set errno, path NULL, return -1
 */
 
static int alloc_append_client_suffix(vfs_handle_struct *handle,
		char **path)
{
	int status = 0;
	DEBUG(UM_INFO_DEBUG, ("Entering with *path '%s'\n", *path));
	
	/* talloc_asprintf_append uses talloc_realloc, which
	 * frees original 'path' memory so we don't have to
	 */
	*path = talloc_asprintf_append(*path, "_%s.%u",
			get_remote_machine_name(), get_digit_group(*path));       
	if (*path == NULL)
	{
		DEBUG(UM_ERR_DEBUG, ("alloc_append_client_suffix "
					"out of memory\n"));
		errno = ENOMEM;
		status = -1;
		goto err;
	}
	DEBUG(UM_INFO_DEBUG, ("Leaving with *path '%s'\n", *path));
err:
	return status;
}

/* Returns True if the file or directory begins with the appledouble
 * prefix.
 */
static bool is_apple_double(const char* fname)
{
	bool ret = False;

	DEBUG(UM_INFO_DEBUG, ("Entering with fname '%s'\n", fname));

	if (strncmp(APPLE_DOUBLE_PREFIX, fname, APPLE_DOUBLE_PREFIX_LEN)
			== 0)
	{
		ret = True;
	}
	DEBUG(UM_INFO_DEBUG, ("Leaving with ret '%s'\n",
				ret == True ? "True" : "False"));
	return ret;
}

static bool starts_with_media_dir(const char* media_dirname,
		size_t media_dirname_len, const char* path)
{
	bool ret = False;
	char* path_start;

	DEBUG(UM_INFO_DEBUG, ("Entering with media_dirname '%s' "
			      "path '%s'\n", media_dirname, path));

	/* Sometimes Samba gives us "./OMFI MediaFiles". */
	if (strncmp(path, "./", 2) == 0)
	{
		path_start = &path[2];
	}
	else {
		path_start = path;
	}

	if (strncmp(media_dirname, path_start, media_dirname_len) == 0
			&&
		(
			path_start[media_dirname_len] == '\0'
			||
			path_start[media_dirname_len] == '/'
		)
	)
	{
		ret = True;
	}

	DEBUG(UM_INFO_DEBUG, ("Leaving with ret '%s'\n",
				ret == True ? "True" : "False"));
	return ret;
}
/*
 * Returns True if the file or directory referenced by the path is ONE LEVEL below
 * the AVID_MXF_DIRNAME or OMFI_MEDIAFILES_DIRNAME directory
 */
static bool is_in_media_dir(const char* path)
{
	int transition_count = 0;
	char* path_start;
	char* pathPtr;
	const char* media_dirname;
	size_t media_dirname_len;
	
	DEBUG(UM_INFO_DEBUG, ("Entering with media_dirname '%s' "
			      "path '%s'\n", media_dirname, path));

	/* Sometimes Samba gives us "./OMFI MediaFiles". */
	if (strncmp(path, "./", 2) == 0)
	{
		path_start = &path[2];
	}
	else {
		path_start = path;
	}

	if (strncmp(path_start, AVID_MXF_DIRNAME, AVID_MXF_DIRNAME_LEN) == 0)
	{
		media_dirname = AVID_MXF_DIRNAME;
		media_dirname_len =AVID_MXF_DIRNAME_LEN;
	}
	else {
	if (strncmp(path_start, OMFI_MEDIAFILES_DIRNAME, OMFI_MEDIAFILES_DIRNAME_LEN) == 0)
	{
		media_dirname = OMFI_MEDIAFILES_DIRNAME;
		media_dirname_len =OMFI_MEDIAFILES_DIRNAME_LEN;
	}
	}
	
	if (path_start[media_dirname_len] == '\0')
	{
		goto out;
	}

	pathPtr = &path_start[media_dirname_len + 1];

	while(1)
	{
		if (*pathPtr == '\0' || *pathPtr == '/')
		{
			if (
				*(pathPtr - 1) == '.'
					&&
				*(pathPtr - 2) == '.'
					&&
				*(pathPtr - 3) == '/'
			)
			{
				transition_count--;
			}
			else if (
				!
				(
					*(pathPtr - 1) == '/'
					||
					(
						*(pathPtr - 1) == '.'
							&&
						*(pathPtr - 2) == '/'
					)
				)
			)
			{
				transition_count++;
			}
		}
		if (*pathPtr == '\0')
		{
			break;
		}
		pathPtr++;
	}

	DEBUG(UM_INFO_DEBUG, ("Leaving with transition_count '%i'\n",
				transition_count));
out:
	DEBUG(UM_INFO_DEBUG, ("Going out with transition_count '%i'\n",
				transition_count));
	if (
		(transition_count == 1
			&&
		media_dirname == AVID_MXF_DIRNAME)
		||
		(transition_count == 0
			&&
		media_dirname == OMFI_MEDIAFILES_DIRNAME) 
		)
		{
		return True;
		}
		else return False;
}
/*
 * Returns True if the file or directory referenced by the path is below
 * the AVID_MEDIAFILES_DIRNAME or OMFI_MEDIAFILES_DIRNAME directory
 * The AVID_MEDIAFILES_DIRNAME and OMFI_MEDIAFILES_DIRNAME are assumed to
 * be in the root directory, which is generally a safe assumption
 * in the fixed-path world of Avid.
 */
static bool is_in_media_files(const char* path)
{
	bool ret = False;

	DEBUG(UM_INFO_DEBUG, ("Entering with path '%s'\n", path));

	if (starts_with_media_dir(AVID_MXF_DIRNAME,
				AVID_MXF_DIRNAME_LEN, path)
		||
		starts_with_media_dir(OMFI_MEDIAFILES_DIRNAME,
				OMFI_MEDIAFILES_DIRNAME_LEN, path)
	)
	{
		ret = True;
	}
	DEBUG(UM_INFO_DEBUG, ("Leaving with ret '%s'\n",
				ret == True ? "True" : "False"));
	return ret;
}


/* Add client suffix to "pure-number" path.
 *
 * Caller must free newPath.
 *
 * Success: return 0
 * Failure: set errno, newPath NULL, return -1
 */
static int alloc_get_client_path(vfs_handle_struct *handle,
		TALLOC_CTX *ctx,
		const char *path,
		char **newPath)
{	
	int status = 0;
	char* pathPtr;
	size_t intermPathLen;
	DEBUG(UM_INFO_DEBUG, ("Entering with path '%s'\n", path));
	*newPath = talloc_strdup(ctx, path);
        if (*newPath == NULL)
	{
		DEBUG(UM_ERR_DEBUG, ("alloc_get_client_path ENOMEM #1\n"));
		errno = ENOMEM;
		status = -1;
		goto out;
	}
	DEBUG(UM_INFO_DEBUG, ("newPath #1 %s\n", *newPath));

	char *digits;
	if ((digits = talloc_asprintf(NULL, "%d", get_digit_group(*newPath))) == NULL) {
			digits = "";
		}
	size_t digits_len= strlen(digits);
	
	DEBUG (UM_INFO_DEBUG, ("digits #2 %s\n", digits));
	if (
          (pathPtr = strstr(path, digits)) != NULL
			&&
		(
			*(pathPtr + digits_len) == '\0'
			||
			*(pathPtr + digits_len) == '/'
		)
			&&
		(
			pathPtr - path > 0
				&&
			*(pathPtr - 1) == '/'
			||
			pathPtr - path > APPLE_DOUBLE_PREFIX_LEN
				&&
			*(pathPtr - APPLE_DOUBLE_PREFIX_LEN - 1) == '/'
				&&
			is_apple_double(pathPtr - APPLE_DOUBLE_PREFIX_LEN)
		)
	)
	{
		DEBUG(UM_INFO_DEBUG, ("preNewPath #1 %s\n", *newPath));
		(*newPath)[pathPtr - path + digits_len] = '\0';
		DEBUG(UM_INFO_DEBUG, ("newPath #2 %s\n", *newPath));

		if ((status = alloc_append_client_suffix(handle, newPath)))
		{
			goto out;
		}

		DEBUG(UM_INFO_DEBUG, ("newPath #3 %s\n", *newPath));
                *newPath = talloc_strdup_append(*newPath,
				pathPtr + digits_len); //here
		if (*newPath == NULL)
		{
			DEBUG(UM_ERR_DEBUG, ("alloc_get_client_path "
						"ENOMEM #2\n"));
			errno = ENOMEM;
			status = -1;
			goto out;
		}
		DEBUG(UM_INFO_DEBUG, ("newPath #4 %s\n", *newPath));
	}
out:
	/* newPath must be freed in caller. */
	DEBUG(UM_INFO_DEBUG, ("Leaving with *newPath '%s'\n", *newPath));
	return status;
}

/*
 * Success: return 0
 * Failure: set errno, return -1
 */
static int alloc_get_client_smb_fname(struct vfs_handle_struct *handle,
		TALLOC_CTX *ctx,
		const struct smb_filename *smb_fname,
		struct smb_filename **clientFname)
{
	int status = 0;
	NTSTATUS copystatus;

	DEBUG(UM_INFO_DEBUG, ("Entering with smb_fname->base_name '%s'\n",
			      smb_fname->base_name));

	copystatus = copy_smb_filename(ctx, smb_fname, clientFname);
	if (!NT_STATUS_IS_OK(copystatus))
	{
		DEBUG(UM_ERR_DEBUG, ("alloc_get_client_smb_fname "
					"NTERR\n"));
		errno = map_errno_from_nt_status(copystatus);
		status = -1;
		goto err;
	}
	if ((status = alloc_get_client_path(handle, ctx,
				smb_fname->base_name,
				&(*clientFname)->base_name)))
	{
		goto err;
	}
	DEBUG(UM_INFO_DEBUG, ("Leaving with (*clientFname)->base_name "
				"'%s'\n", (*clientFname)->base_name));
err:
	return status;
}


/*
 * Success: return 0
 * Failure: set errno, return -1
 */
static int alloc_set_client_dirinfo_path(struct vfs_handle_struct *handle,
		TALLOC_CTX *ctx,
		char **path,
		const char *suffix_number)
{
	int status = 0;

	DEBUG(UM_INFO_DEBUG, ("Entering with suffix_number '%s'\n",
			      suffix_number));

	if ((*path = talloc_strdup(ctx, suffix_number)) == NULL)
	{
		DEBUG(UM_ERR_DEBUG, ("alloc_set_client_dirinfo_path "
					"ENOMEM\n"));
		errno = ENOMEM;
		status = -1;
		goto err;
	}
	if ((status = alloc_append_client_suffix(handle, path)))
	{
		goto err;
	}
	DEBUG(UM_INFO_DEBUG, ("Leaving with *path '%s'\n", *path));
err:
	return status;
}
/*
 * Success: return 0
 * Failure: set errno, return -1
 */
static int um_statvfs(struct vfs_handle_struct *handle,
		const char *path,
		struct vfs_statvfs_struct *statbuf)
{
	int status;
	char *clientPath;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering with path '%s'\n", path));

	if 	(!is_in_media_files(path))
	{
		status = SMB_VFS_NEXT_STATVFS(handle, path, statbuf);
		goto out;
	}

	clientPath = NULL;
	ctx = talloc_tos();

	if ((status = alloc_get_client_path(handle, ctx,
				path,
				&clientPath)))
	{
		goto err;
	}

	status = SMB_VFS_NEXT_STATVFS(handle, clientPath, statbuf);
err:
	TALLOC_FREE(clientPath);
out:
	DEBUG(UM_INFO_DEBUG, ("Leaving with path '%s'\n", path));
	return status;
}

static int alloc_set_client_dirinfo(vfs_handle_struct *handle,
		const char *fname,
		struct um_dirinfo_struct **dirInfo)
{
	int status = 0;
	char *clientPath;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering with fname '%s'\n", fname));

	*dirInfo = talloc(NULL, struct um_dirinfo_struct);
	if (*dirInfo == NULL)
	{
		goto err;
	}

	(*dirInfo)->dirpath = talloc_strdup(*dirInfo, fname);
	if ((*dirInfo)->dirpath == NULL)
	{
		goto err;
	}

	if 	(!is_in_media_files(fname))
	{
		(*dirInfo)->clientPath = NULL;
		(*dirInfo)->clientSubDirname = NULL;
		(*dirInfo)->isInMediaFiles = False;
		goto out;
	}
	
	(*dirInfo)->isInMediaFiles = True;
	
	// get suffix
	char *digits;
	if ((digits = talloc_asprintf(NULL, "%d", get_digit_group(fname))) == NULL) {
			digits = "";
		}
	size_t digits_len= strlen(digits);
	
	if (alloc_set_client_dirinfo_path(handle,
				*dirInfo,
				&((*dirInfo)->clientSubDirname),
				digits))
	{
		goto err;
	}

	clientPath = NULL;
	ctx = talloc_tos();

	if (alloc_get_client_path(handle, ctx,
				fname,
				&clientPath))
	{
		goto err;
	}

	(*dirInfo)->clientPath = talloc_strdup(*dirInfo, clientPath);
	if ((*dirInfo)->clientPath == NULL)
	{
		goto err;
	}

	TALLOC_FREE(clientPath);

out:
	DEBUG(UM_INFO_DEBUG, ("Leaving with (*dirInfo)->dirpath '%s', "
				"(*dirInfo)->clientPath '%s'\n",
				(*dirInfo)->dirpath,
				(*dirInfo)->clientPath));
	return status;

err:
	DEBUG(UM_ERR_DEBUG, ("Failing with fname '%s'\n", fname));
	TALLOC_FREE(*dirInfo);
	status = -1;
	errno = ENOMEM;
	return status;
}

/* Success: return a um_dirinfo_struct cast as a DIR
 * Failure: set errno, return NULL
 */
static DIR *um_opendir(vfs_handle_struct *handle,
		const char *fname,
		const char *mask,
		uint32 attr)
{
	struct um_dirinfo_struct *dirInfo;

	DEBUG(UM_INFO_DEBUG, ("Entering with fname '%s'\n", fname));

	if (alloc_set_client_dirinfo(handle, fname, &dirInfo))
	{
		goto err;
	}

	if (!dirInfo->isInMediaFiles)
	{
		dirInfo->dirstream = SMB_VFS_NEXT_OPENDIR(handle,
			fname, mask, attr);
	} else {
		dirInfo->dirstream = SMB_VFS_NEXT_OPENDIR(handle,
			dirInfo->clientPath, mask, attr);
	}

	if (dirInfo->dirstream == NULL) {
		goto err;
	}

out:
	/* Success is freed in closedir. */
	DEBUG(UM_INFO_DEBUG, ("Leaving with dirInfo->dirpath '%s', "
				"dirInfo->clientPath '%s'\n",
				dirInfo->dirpath,
				dirInfo->clientPath));
	return (DIR*)dirInfo;
err:
	/* Failure is freed here. */
	DEBUG(UM_ERR_DEBUG, ("Failing with fname '%s'\n", fname));
	TALLOC_FREE(dirInfo);
	return NULL;
}

static DIR *um_fdopendir(vfs_handle_struct *handle,
		files_struct *fsp,
		const char *mask,
		uint32 attr)
{
	struct um_dirinfo_struct *dirInfo = NULL;
	DIR *dirstream;

	DEBUG(UM_INFO_DEBUG, ("Entering with fsp->fsp_name->base_name '%s'\n",
			      fsp->fsp_name->base_name));

	dirstream = SMB_VFS_NEXT_FDOPENDIR(handle, fsp, mask, attr);
	if (!dirstream)
	{
		goto err;
	}

	if (alloc_set_client_dirinfo(handle, fsp->fsp_name->base_name,
					&dirInfo))
	{
		goto err;
	}

	dirInfo->dirstream = dirstream;

	if (! dirInfo->isInMediaFiles) {
		goto out;
	}

out:
	DEBUG(UM_INFO_DEBUG, ("Leaving with dirInfo->dirpath '%s', "
			"dirInfo->clientPath '%s', "
			"fsp->fsp_name->st.st_ex_mtime %s",
			dirInfo->dirpath,
			dirInfo->clientPath,
			ctime(&(fsp->fsp_name->st.st_ex_mtime.tv_sec))));
	/* Success is freed in closedir. */
	return (DIR *) dirInfo;
err:
	/* Failure is freed here. */
	DEBUG(UM_ERR_DEBUG, ("Failing with fsp->fsp_name->base_name '%s'\n",
			fsp->fsp_name->base_name));
	TALLOC_FREE(dirInfo);
	return NULL;
}

/*
 * skip own suffixed directory
 * replace own suffixed directory with non suffixed.
 *
 * Success: return dirent
 * End of data: return NULL
 * Failure: set errno, return NULL
 */
static struct dirent *um_readdir(vfs_handle_struct *handle,
		DIR *dirp,
		SMB_STRUCT_STAT *sbuf)
{
	um_dirinfo_struct* dirInfo = (um_dirinfo_struct*)dirp;
	struct dirent *d = NULL;
	int skip;

	DEBUG(UM_INFO_DEBUG, ("Entering um_readdir\n"));

	DEBUG(UM_INFO_DEBUG, ("dirInfo->dirpath '%s', "
			      "dirInfo->clientPath '%s', "
			      "dirInfo->isInMediaFiles '%s', "
			      "dirInfo->clientSubDirname '%s'\n",
			      dirInfo->dirpath,
			      dirInfo->clientPath,
			      dirInfo->isInMediaFiles ? "True" : "False",
			      dirInfo->clientSubDirname));

	if (! dirInfo->isInMediaFiles)
	{
		d = SMB_VFS_NEXT_READDIR(handle, dirInfo->dirstream, sbuf);
		goto out;
	}

	do
	{
		const char* dname;
		bool isAppleDouble;

		skip = False;
		d = SMB_VFS_NEXT_READDIR(handle, dirInfo->dirstream, sbuf);

		if (d == NULL)
		{
			break;
		}

		/* ignore apple double prefix for logic below */
		if (is_apple_double(d->d_name))
		{
			dname = &d->d_name[APPLE_DOUBLE_PREFIX_LEN];
			isAppleDouble = True;
		}
		else
		{
			dname = d->d_name;
			isAppleDouble = False;
		}
		
		DEBUG(UM_INFO_DEBUG, ("dname = '%s'\n", dname));
		
		char *digits;
		if ((digits = talloc_asprintf(NULL, "%d", get_digit_group(dname))) == NULL) {
			digits = "";
		}
		size_t digits_len= strlen(digits);
				
		if (alloc_set_client_dirinfo_path(handle,
				dirInfo,
				&((dirInfo)->clientSubDirname),
				digits))
		{
		goto err;
		}
		
		/* if set to "True", vfs shows digits-only 
		 * non-suffixed subdirectories.  
		 * Normally, such subdirectories can exists only in non-media 
		 * directories, so we set it to "False". 
		 * Otherwise, if we have such subdirectories (probably created 
		 * over not "unityed" connection), it can be little bit confusing...*/
		if (
			strcmp(dname, digits) == 0
		)
		{
			skip = False;
		}
		/* remove suffix of this client's suffixed subdirectories*/
		else if (strcmp(dname, dirInfo->clientSubDirname)
				== 0)
		{
			if (isAppleDouble)
			{
				d->d_name[digits_len
					+ APPLE_DOUBLE_PREFIX_LEN] = '\0';
			}
			else
			{
				d->d_name[digits_len] = '\0';
			}
		}
		
		/*
		 * Set to False to see another clients subdirectories 
		 */
		else if (
			strncmp(digits, dname,
				digits_len) == 0
		)
		{
			skip = False;
		}
	}
	while (skip);

out:
	DEBUG(UM_INFO_DEBUG, ("Leaving um_readdir\n"));
	return d;
err:
	/* Failure 1 is freed here. */
	TALLOC_FREE(dirInfo);
	return NULL;
}

/*
 * Success: no success result defined.
 * Failure: no failure result defined.
 */
static void um_seekdir(vfs_handle_struct *handle,
		DIR *dirp,
		long offset)
{
	DEBUG(UM_INFO_DEBUG, ("Entering and leaving um_seekdir\n"));
	SMB_VFS_NEXT_SEEKDIR(handle,
			((um_dirinfo_struct*)dirp)->dirstream, offset);
}

/*
 * Success: return long
 * Failure: no failure result defined.
 */
static long um_telldir(vfs_handle_struct *handle,
		DIR *dirp)
{
	DEBUG(UM_INFO_DEBUG, ("Entering and leaving um_telldir\n"));
	return SMB_VFS_NEXT_TELLDIR(handle,
			((um_dirinfo_struct*)dirp)->dirstream);
}

/*
 * Success: no success result defined.
 * Failure: no failure result defined.
 */
static void um_rewinddir(vfs_handle_struct *handle,
		DIR *dirp)
{
	DEBUG(UM_INFO_DEBUG, ("Entering and leaving um_rewinddir\n"));
	SMB_VFS_NEXT_REWINDDIR(handle,
			((um_dirinfo_struct*)dirp)->dirstream);
}

/*
 * Success: return 0
 * Failure: set errno, return -1
 */
static int um_mkdir(vfs_handle_struct *handle,
		const char *path,
		mode_t mode)
{
	int status;
	char *clientPath;
	TALLOC_CTX *ctx;


	DEBUG(UM_INFO_DEBUG, ("Entering with path '%s'\n", path));

	if (!is_in_media_files(path)
	||
	(!is_in_media_dir(path))
	)
	{
		status = SMB_VFS_NEXT_MKDIR(handle, path, mode);
		goto out;
	}

	clientPath = NULL;
	ctx = talloc_tos();

	if ((status = alloc_get_client_path(handle, ctx,
				path,
				&clientPath)))
	{
		goto err;
	}

	status = SMB_VFS_NEXT_MKDIR(handle, clientPath, mode);
err:
	TALLOC_FREE(clientPath);
out:
	DEBUG(UM_INFO_DEBUG, ("Leaving with path '%s'\n", path));
	return status;
}

/*
 * Success: return 0
 * Failure: set errno, return -1
 */
static int um_rmdir(vfs_handle_struct *handle,
		const char *path)
{
	int status;
	char *clientPath;
	TALLOC_CTX *ctx;


	DEBUG(UM_INFO_DEBUG, ("Entering with path '%s'\n", path));

	if (!is_in_media_files(path))
	{
		status = SMB_VFS_NEXT_RMDIR(handle, path);
		goto out;
	}

	clientPath = NULL;
	ctx = talloc_tos();

	if ((status = alloc_get_client_path(handle, ctx,
				path,
				&clientPath)))
	{
		goto err;
	}

	status = SMB_VFS_NEXT_RMDIR(handle, clientPath);
err:
	TALLOC_FREE(clientPath);
out:
	DEBUG(UM_INFO_DEBUG, ("Leaving with path '%s'\n", path));
	return status;
}

/*
 * Success: return 0
 * Failure: set errno, return -1
 */
static int um_closedir(vfs_handle_struct *handle,
		DIR *dirp)
{
	DIR *realdirp = ((um_dirinfo_struct*)dirp)->dirstream;

	DEBUG(UM_INFO_DEBUG, ("Entering um_closedir\n"));
	// Will this talloc_free destroy realdirp?
	TALLOC_FREE(dirp);

	DEBUG(UM_INFO_DEBUG, ("Leaving um_closedir\n"));
	return SMB_VFS_NEXT_CLOSEDIR(handle, realdirp);
}

/*
 * Success: no success result defined.
 * Failure: no failure result defined.
 */
static void um_init_search_op(vfs_handle_struct *handle,
		DIR *dirp)
{
	DEBUG(UM_INFO_DEBUG, ("Entering and leaving um_init_search_op\n"));
	SMB_VFS_NEXT_INIT_SEARCH_OP(handle,
			((um_dirinfo_struct*)dirp)->dirstream);
}

/*
 * Success: return non-negative file descriptor
 * Failure: set errno, return -1
 */
static int um_open(vfs_handle_struct *handle,
		struct smb_filename *smb_fname,
		files_struct *fsp,
		int flags,
		mode_t mode)
{
	int ret;
	struct smb_filename *clientFname;
	TALLOC_CTX *ctx;


	DEBUG(UM_INFO_DEBUG, ("Entering with smb_fname->base_name '%s'\n",
			      smb_fname->base_name));

	if (!is_in_media_files(smb_fname->base_name))
	{
		ret = SMB_VFS_NEXT_OPEN(handle, smb_fname, fsp, flags,
				mode);
		goto out;
	}

	clientFname = NULL;
	ctx = talloc_tos();

	if(alloc_get_client_smb_fname(handle, ctx,
				smb_fname,
				&clientFname))
	{
		ret = -1;
		goto err;
	}

	// What about fsp->fsp_name?
	// We also have to get correct stat info into fsp and smb_fname
	// for DB files, don't we?

	DEBUG(UM_INFO_DEBUG, ("Leaving with smb_fname->base_name '%s' "
			"smb_fname->st.st_ex_mtime %s"
			"		fsp->fsp_name->st.st_ex_mtime %s",
			smb_fname->base_name,
			ctime(&(smb_fname->st.st_ex_mtime.tv_sec)),
			ctime(&(fsp->fsp_name->st.st_ex_mtime.tv_sec))));

	ret = SMB_VFS_NEXT_OPEN(handle, clientFname, fsp, flags, mode);
err:
	TALLOC_FREE(clientFname);
out:
	DEBUG(UM_INFO_DEBUG, ("Leaving with smb_fname->base_name '%s'\n",
				smb_fname->base_name));
	return ret;
}

/*
 * Success: return non-negative file descriptor
 * Failure: set errno, return -1
 */
static NTSTATUS um_create_file(vfs_handle_struct *handle,
		struct smb_request *req,
		uint16_t root_dir_fid,
		struct smb_filename *smb_fname,
		uint32_t access_mask,
		uint32_t share_access,
		uint32_t create_disposition,
		uint32_t create_options,
		uint32_t file_attributes,
		uint32_t oplock_request,
		uint64_t allocation_size,
		uint32_t private_flags,
		struct security_descriptor *sd,
		struct ea_list *ea_list,
		files_struct **result_fsp,
		int *pinfo)
{
	NTSTATUS status;
	struct smb_filename *clientFname;
	TALLOC_CTX *ctx;


	DEBUG(UM_INFO_DEBUG, ("Entering with smb_fname->base_name '%s'\n",
				smb_fname->base_name));
	if (!is_in_media_files(smb_fname->base_name))
	{
		status = SMB_VFS_NEXT_CREATE_FILE(
			handle,
			req,
			root_dir_fid,
			smb_fname,
			access_mask,
			share_access,
			create_disposition,
			create_options,
			file_attributes,
			oplock_request,
			allocation_size,
			private_flags,
			sd,
			ea_list,
			result_fsp,
			pinfo);
		goto out;
	}

	clientFname = NULL;
	ctx = talloc_tos();

	if (alloc_get_client_smb_fname(handle, ctx,
				smb_fname,
				&clientFname))
	{
		status = map_nt_error_from_unix(errno);
		goto err;
	}

	/* This only creates files, so we don't have to worry about
	 * our fake directory stat'ing here.
	 */
	// But we still need to route stat calls for DB files
	// properly, right?
	status = SMB_VFS_NEXT_CREATE_FILE(
		handle,
		req,
		root_dir_fid,
		clientFname,
		access_mask,
		share_access,
		create_disposition,
		create_options,
		file_attributes,
		oplock_request,
		allocation_size,
		private_flags,
		sd,
		ea_list,
		result_fsp,
		pinfo);
err:
	TALLOC_FREE(clientFname);
out:
	DEBUG(UM_INFO_DEBUG, ("Leaving with smb_fname->base_name '%s'"
		"smb_fname->st.st_ex_mtime %s"
		"		fsp->fsp_name->st.st_ex_mtime %s",
		smb_fname->base_name,
		ctime(&(smb_fname->st.st_ex_mtime.tv_sec)),
		(*result_fsp) && VALID_STAT((*result_fsp)->fsp_name->st) ?
		ctime(&((*result_fsp)->fsp_name->st.st_ex_mtime.tv_sec)) :
		"No fsp time\n"));
	return status;
}

/*
 * Success: return 0
 * Failure: set errno, return -1
 */
static int um_rename(vfs_handle_struct *handle,
		const struct smb_filename *smb_fname_src,
		const struct smb_filename *smb_fname_dst)
{
	int status;
	struct smb_filename *srcClientFname;
	struct smb_filename *dstClientFname;
	TALLOC_CTX *ctx;


	DEBUG(UM_INFO_DEBUG, ("Entering with "
			      "smb_fname_src->base_name '%s', "
			      "smb_fname_dst->base_name '%s'\n",
			      smb_fname_src->base_name,
			      smb_fname_dst->base_name));

	if (!is_in_media_files(smb_fname_src->base_name)
				&&
			!is_in_media_files(smb_fname_dst->base_name))
	{
		status = SMB_VFS_NEXT_RENAME(handle, smb_fname_src,
				smb_fname_dst);
		goto out;
	}

	srcClientFname = NULL;
	dstClientFname = NULL;
	ctx = talloc_tos();

	if ((status = alloc_get_client_smb_fname(handle, ctx,
				smb_fname_src,
				&srcClientFname)))
	{
		goto err;
	}

	if ((status = alloc_get_client_smb_fname(handle, ctx,
				smb_fname_dst,
				&dstClientFname)))
	{
		goto err;
	}

	status = SMB_VFS_NEXT_RENAME(handle, srcClientFname,
				dstClientFname);
err:
	TALLOC_FREE(dstClientFname);
	TALLOC_FREE(srcClientFname);
out:
	DEBUG(UM_INFO_DEBUG, ("Leaving with smb_fname_src->base_name '%s',"
				" smb_fname_dst->base_name '%s'\n",
				smb_fname_src->base_name,
				smb_fname_dst->base_name));
	return status;
}

/*
 * Success: return 0
 * Failure: set errno, return -1
 */
static int um_stat(vfs_handle_struct *handle,
		struct smb_filename *smb_fname)
{
	int status = 0;
	struct smb_filename *clientFname;
	TALLOC_CTX *ctx;


	DEBUG(UM_INFO_DEBUG, ("Entering with smb_fname->base_name '%s'\n",
			      smb_fname->base_name));

	if (!is_in_media_files(smb_fname->base_name))
	{
		status = SMB_VFS_NEXT_STAT(handle, smb_fname);
		goto out;
	}

	clientFname = NULL;
	ctx = talloc_tos();

	if ((status = alloc_get_client_smb_fname(handle, ctx,
				smb_fname,
				&clientFname)))
	{
		goto err;
	}
	DEBUG(UM_INFO_DEBUG, ("Stat'ing clientFname->base_name '%s'\n",
				clientFname->base_name));
	if ((status = SMB_VFS_NEXT_STAT(handle, clientFname)))
	{
		goto err;
	}

	/* Unlike functions with const smb_filename, we have to
	 * modify smb_fname itself to pass our info back up.
	 */
	DEBUG(UM_INFO_DEBUG, ("Setting smb_fname '%s' stat "
				"from clientFname '%s'\n",
				smb_fname->base_name,
				clientFname->base_name));
	smb_fname->st = clientFname->st;
err:
	TALLOC_FREE(clientFname);
out:
	DEBUG(UM_INFO_DEBUG, ("Leaving with smb_fname->st.st_ex_mtime %s",
			ctime(&(smb_fname->st.st_ex_mtime.tv_sec))));
	return status;
}

/*
 * Success: return 0
 * Failure: set errno, return -1
 */
static int um_lstat(vfs_handle_struct *handle,
		struct smb_filename *smb_fname)
{
	int status = 0;
	struct smb_filename *clientFname;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering with smb_fname->base_name '%s'\n",
			      smb_fname->base_name));

	if (!is_in_media_files(smb_fname->base_name))
	{
		status = SMB_VFS_NEXT_LSTAT(handle, smb_fname);
		goto out;
	}

	clientFname = NULL;
	ctx = talloc_tos();

	if ((status = alloc_get_client_smb_fname(handle, ctx,
				smb_fname,
				&clientFname)))
	{
		goto err;
	}
	if ((status = SMB_VFS_NEXT_LSTAT(handle, clientFname)))
	{
		goto err;
	}

	/* Unlike functions with const smb_filename, we have to
	 * modify smb_fname itself to pass our info back up.
	 */
	smb_fname->st = clientFname->st;
err:
	TALLOC_FREE(clientFname);
out:
	DEBUG(UM_INFO_DEBUG, ("Leaving with smb_fname->st.st_ex_mtime %s",
			ctime(&(smb_fname->st.st_ex_mtime.tv_sec))));
	return status;
}

/*
 * Success: return 0
 * Failure: set errno, return -1
 */
static int um_fstat(vfs_handle_struct *handle,
		files_struct *fsp, SMB_STRUCT_STAT *sbuf)
{
	int status = 0;

	DEBUG(UM_INFO_DEBUG, ("Entering with fsp->fsp_name->base_name "
				"'%s'\n", fsp_str_dbg(fsp)));

	if ((status = SMB_VFS_NEXT_FSTAT(handle, fsp, sbuf)))
	{
		goto out;
	}

	if (fsp->fsp_name == NULL
			|| !is_in_media_files(fsp->fsp_name->base_name))
	{
		goto out;
	}

	if ((status = um_stat(handle, fsp->fsp_name)))
	{
		goto out;
	}

	*sbuf = fsp->fsp_name->st;
out:
	DEBUG(UM_INFO_DEBUG, ("Leaving with fsp->fsp_name->st.st_ex_mtime "
			"%s",
			fsp->fsp_name != NULL ?
				ctime(&(fsp->fsp_name->st.st_ex_mtime.tv_sec)) :
				"0"));
	return status;
}

/*
 * Success: return 0
 * Failure: set errno, return -1
 */
static int um_unlink(vfs_handle_struct *handle,
		const struct smb_filename *smb_fname)
{
	int status;
	struct smb_filename *clientFname;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering um_unlink\n"));
	if (!is_in_media_files(smb_fname->base_name))
	{
		status = SMB_VFS_NEXT_UNLINK(handle, smb_fname);
		goto out;
	}

	clientFname = NULL;
	ctx = talloc_tos();

	if ((status = alloc_get_client_smb_fname(handle, ctx,
				smb_fname,
				&clientFname)))
	{
		goto err;
	}

	status = SMB_VFS_NEXT_UNLINK(handle, clientFname);
err:
	TALLOC_FREE(clientFname);
out:
	return status;
}

/*
 * Success: return 0
 * Failure: set errno, return -1
 */
static int um_chmod(vfs_handle_struct *handle,
		const char *path,
		mode_t mode)
{
	int status;
	char *clientPath;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering um_chmod\n"));
	if (!is_in_media_files(path))
	{
		status = SMB_VFS_NEXT_CHMOD(handle, path, mode);
		goto out;
	}

	clientPath = NULL;
	ctx = talloc_tos();

	if ((status = alloc_get_client_path(handle, ctx,
				path,
				&clientPath)))
	{
		goto err;
	}

	status = SMB_VFS_NEXT_CHMOD(handle, clientPath, mode);
err:
	TALLOC_FREE(clientPath);
out:
	return status;
}

/*
 * Success: return 0
 * Failure: set errno, return -1
 */
static int um_chown(vfs_handle_struct *handle,
		const char *path,
		uid_t uid,
		gid_t gid)
{
	int status;
	char *clientPath;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering um_chown\n"));
	if (!is_in_media_files(path))
	{
		status = SMB_VFS_NEXT_CHOWN(handle, path, uid, gid);
		goto out;
	}

	clientPath = NULL;
	ctx = talloc_tos();

	if ((status = alloc_get_client_path(handle, ctx,
				path,
				&clientPath)))
	{
		goto err;
	}

	status = SMB_VFS_NEXT_CHOWN(handle, clientPath, uid, gid);
err:
	TALLOC_FREE(clientPath);
out:
	return status;
}

/*
 * Success: return 0
 * Failure: set errno, return -1
 */
static int um_lchown(vfs_handle_struct *handle,
		const char *path,
		uid_t uid,
		gid_t gid)
{
	int status;
	char *clientPath;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering um_lchown\n"));
	if (!is_in_media_files(path))
	{
		status = SMB_VFS_NEXT_LCHOWN(handle, path, uid, gid);
		goto out;
	}

	clientPath = NULL;
	ctx = talloc_tos();

	if ((status = alloc_get_client_path(handle, ctx,
				path,
				&clientPath)))
	{
		goto err;
	}

	status = SMB_VFS_NEXT_LCHOWN(handle, clientPath, uid, gid);
err:
	TALLOC_FREE(clientPath);
out:
	return status;
}

/*
 * Success: return 0
 * Failure: set errno, return -1
 */
static int um_chdir(vfs_handle_struct *handle,
		const char *path)
{
	int status;
	char *clientPath;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering um_chdir\n"));
	if (!is_in_media_files(path))
	{
		status = SMB_VFS_NEXT_CHDIR(handle, path);
		goto out;
	}

	clientPath = NULL;
	ctx = talloc_tos();

	if ((status = alloc_get_client_path(handle, ctx,
				path,
				&clientPath)))
	{
		goto err;
	}

	status = SMB_VFS_NEXT_CHDIR(handle, clientPath);
err:
	TALLOC_FREE(clientPath);
out:
	return status;
}

/*
 * Success: return 0
 * Failure: set errno, return -1
 */
static int um_ntimes(vfs_handle_struct *handle,
		const struct smb_filename *smb_fname,
		struct smb_file_time *ft)
{
	int status;
	struct smb_filename *clientFname;
	TALLOC_CTX *ctx;


	DEBUG(UM_INFO_DEBUG, ("Entering um_ntimes\n"));
	if (!is_in_media_files(smb_fname->base_name))
	{
		status = SMB_VFS_NEXT_NTIMES(handle, smb_fname, ft);
		goto out;
	}

	clientFname = NULL;
	ctx = talloc_tos();

	if ((status = alloc_get_client_smb_fname(handle, ctx,
				smb_fname,
				&clientFname)))
	{
		goto err;
	}

	status = SMB_VFS_NEXT_NTIMES(handle, clientFname, ft);
err:
	TALLOC_FREE(clientFname);
out:
	return status;
}

/*
 * Success: return 0
 * Failure: set errno, return -1
 */
static int um_symlink(vfs_handle_struct *handle,
		const char *oldpath,
		const char *newpath)
{
	int status;
	char *oldClientPath;
	char *newClientPath;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering um_symlink\n"));
	if (!is_in_media_files(oldpath) && !is_in_media_files(newpath))
	{
		status = SMB_VFS_NEXT_SYMLINK(handle, oldpath, newpath);
		goto out;
	}

	oldClientPath = NULL;
	newClientPath = NULL;
	ctx = talloc_tos();

	if ((status = alloc_get_client_path(handle, ctx,
				oldpath,
				&oldClientPath)))
	{
		goto err;
	}

	if ((status = alloc_get_client_path(handle, ctx,
				newpath,
				&newClientPath)))
	{
		goto err;
	}

	status = SMB_VFS_NEXT_SYMLINK(handle,
			oldClientPath,
			newClientPath);

err:
	TALLOC_FREE(newClientPath);
	TALLOC_FREE(oldClientPath);
out:
	return status;
}

/*
 * Success: return byte count
 * Failure: set errno, return -1
 */
static int um_readlink(vfs_handle_struct *handle,
		const char *path,
		char *buf,
		size_t bufsiz)
{
	int status;
	char *clientPath;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering um_readlink\n"));
	if (!is_in_media_files(path))
	{
		status = SMB_VFS_NEXT_READLINK(handle, path, buf, bufsiz);
		goto out;
	}

	clientPath = NULL;
	ctx = talloc_tos();

	if ((status = alloc_get_client_path(handle, ctx,
				path,
				&clientPath)))
	{
		goto err;
	}

	status = SMB_VFS_NEXT_READLINK(handle, clientPath, buf, bufsiz);
err:
	TALLOC_FREE(clientPath);
out:
	return status;
}

/*
 * Success: return 0
 * Failure: set errno, return -1
 */
static int um_link(vfs_handle_struct *handle,
		const char *oldpath,
		const char *newpath)
{
	int status;
	char *oldClientPath;
	char *newClientPath;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering um_link\n"));
	if (!is_in_media_files(oldpath) && !is_in_media_files(newpath))
	{
		status = SMB_VFS_NEXT_LINK(handle, oldpath, newpath);
		goto out;
	}

	oldClientPath = NULL;
	newClientPath = NULL;
	ctx = talloc_tos();

	if ((status = alloc_get_client_path(handle, ctx,
				oldpath,
				&oldClientPath)))
	{
		goto err;
	}

	if ((status = alloc_get_client_path(handle, ctx,
				newpath,
				&newClientPath)))
	{
		goto err;
	}

	status = SMB_VFS_NEXT_LINK(handle, oldClientPath, newClientPath);
err:
	TALLOC_FREE(newClientPath);
	TALLOC_FREE(oldClientPath);
out:
	return status;
}

/*
 * Success: return 0
 * Failure: set errno, return -1
 */
static int um_mknod(vfs_handle_struct *handle,
		const char *pathname,
		mode_t mode,
		SMB_DEV_T dev)
{
	int status;
	char *clientPath;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering um_mknod\n"));
	if (!is_in_media_files(pathname))
	{
		status = SMB_VFS_NEXT_MKNOD(handle, pathname, mode, dev);
		goto out;
	}

	clientPath = NULL;
	ctx = talloc_tos();

	if ((status = alloc_get_client_path(handle, ctx,
				pathname,
				&clientPath)))
	{
		goto err;
	}

	status = SMB_VFS_NEXT_MKNOD(handle, clientPath, mode, dev);
err:
	TALLOC_FREE(clientPath);
out:
	return status;
}

/*
 * Success: return path pointer
 * Failure: set errno, return NULL pointer
 */
static char *um_realpath(vfs_handle_struct *handle,
		const char *path)
{
	char *buf;
	char *clientPath;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering um_realpath\n"));
	if (!is_in_media_files(path))
	{
		buf = SMB_VFS_NEXT_REALPATH(handle, path);
		goto out;
	}

	clientPath = NULL;
	ctx = talloc_tos();

	if (alloc_get_client_path(handle, ctx,
				path,
				&clientPath))
	{
		buf = NULL;
		goto err;
	}

	buf = SMB_VFS_NEXT_REALPATH(handle, clientPath);
err:
	TALLOC_FREE(clientPath);
out:
	return buf;
}

/*
 * Success: return 0
 * Failure: set errno, return -1
 */
static int um_chflags(vfs_handle_struct *handle,
		const char *path,
		unsigned int flags)
{
	int status;
	char *clientPath;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering um_chflags\n"));
	if (!is_in_media_files(path))
	{
		status = SMB_VFS_NEXT_CHFLAGS(handle, path, flags);
		goto out;
	}

	clientPath = NULL;
	ctx = talloc_tos();

	if ((status = alloc_get_client_path(handle, ctx,
				path,
				&clientPath)))
	{
		goto err;
	}

	status = SMB_VFS_NEXT_CHFLAGS(handle, clientPath, flags);
err:
	TALLOC_FREE(clientPath);
out:
	return status;
}

/*
 * Success: return NT_STATUS_OK
 * Failure: return NT status error
 */
static NTSTATUS um_streaminfo(struct vfs_handle_struct *handle,
		struct files_struct *fsp,
		const char *fname,
		TALLOC_CTX *ctx,
		unsigned int *num_streams,
		struct stream_struct **streams)
{
	NTSTATUS status;
	char *clientPath;
	TALLOC_CTX *mem_ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering um_streaminfo\n"));
	if (!is_in_media_files(fname))
	{
		status = SMB_VFS_NEXT_STREAMINFO(handle, fsp, fname,
				ctx, num_streams, streams);
		goto out;
	}

	clientPath = NULL;
	mem_ctx = talloc_tos();

	if (alloc_get_client_path(handle, mem_ctx,
				fname,
				&clientPath))
	{
		status = map_nt_error_from_unix(errno);
		goto err;
	}

	/* This only works on files, so we don't have to worry about
	 * our fake directory stat'ing here.
	 */
	// But what does this function do, exactly?  Does it need
	// extra modifications for the Avid stuff?
	status = SMB_VFS_NEXT_STREAMINFO(handle, fsp, clientPath,
				ctx, num_streams, streams);
err:
	TALLOC_FREE(clientPath);
out:
	return status;
}

/* Ignoring get_real_filename function because the default
 * doesn't do anything.
 */

/*
 * Success: return NT_STATUS_OK
 * Failure: return NT status error
 * In this case, "name" is a path.
 */
static NTSTATUS um_get_nt_acl(vfs_handle_struct *handle,
			      const char *name,
			      uint32 security_info,
			      TALLOC_CTX *mem_ctx,
			      struct security_descriptor **ppdesc)
{
	NTSTATUS status;
	char *clientPath;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering um_get_nt_acl\n"));
	if (!is_in_media_files(name))
	{
		status = SMB_VFS_NEXT_GET_NT_ACL(handle, name,
						 security_info,
						 mem_ctx, ppdesc);
		goto out;
	}

	clientPath = NULL;
	ctx = talloc_tos();

	if (alloc_get_client_path(handle, ctx,
				name,
				&clientPath))
	{
		status = map_nt_error_from_unix(errno);
		goto err;
	}

	status = SMB_VFS_NEXT_GET_NT_ACL(handle, clientPath,
					 security_info,
					 mem_ctx, ppdesc);
err:
	TALLOC_FREE(clientPath);
out:
	return status;
}

/*
 * Success: return 0
 * Failure: set errno, return -1
 */
static int um_chmod_acl(vfs_handle_struct *handle,
		const char *path,
		mode_t mode)
{
	int status;
	char *clientPath;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering um_chmod_acl\n"));
	if (!is_in_media_files(path))
	{
		status = SMB_VFS_NEXT_CHMOD_ACL(handle, path, mode);
		goto out;
	}

	clientPath = NULL;
	ctx = talloc_tos();

	if ((status = alloc_get_client_path(handle, ctx,
				path,
				&clientPath)))
	{
		goto err;
	}

	status = SMB_VFS_NEXT_CHMOD_ACL(handle, clientPath, mode);
err:
	TALLOC_FREE(clientPath);
out:
	return status;
}

/*
 * Success: return acl pointer
 * Failure: set errno, return NULL
 */
static SMB_ACL_T um_sys_acl_get_file(vfs_handle_struct *handle,
				     const char *path_p,
				     SMB_ACL_TYPE_T type,
				     TALLOC_CTX *mem_ctx)
{
	SMB_ACL_T ret;
	char *clientPath;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering um_sys_acl_get_file\n"));
	if (!is_in_media_files(path_p))
	{
		ret = SMB_VFS_NEXT_SYS_ACL_GET_FILE(handle, path_p, type, mem_ctx);
		goto out;
	}

	clientPath = NULL;
	ctx = talloc_tos();

	if (alloc_get_client_path(handle, ctx,
				path_p,
				&clientPath))
	{
		ret = NULL;
		goto err;
	}

	ret = SMB_VFS_NEXT_SYS_ACL_GET_FILE(handle, clientPath, type, mem_ctx);
err:
	TALLOC_FREE(clientPath);
out:
	return ret;
}

/*
 * Success: return 0
 * Failure: set errno, return -1
 * In this case, "name" is a path.
 */
static int um_sys_acl_set_file(vfs_handle_struct *handle,
		const char *name,
		SMB_ACL_TYPE_T acltype,
		SMB_ACL_T theacl)
{
	int status;
	char *clientPath;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering um_sys_acl_set_file\n"));
	if (!is_in_media_files(name))
	{
		status = SMB_VFS_NEXT_SYS_ACL_SET_FILE(handle, name,
				acltype, theacl);
		goto out;
	}

	clientPath = NULL;
	ctx = talloc_tos();

	if ((status = alloc_get_client_path(handle, ctx,
				name,
				&clientPath)))
	{
		goto err;
	}

	status = SMB_VFS_NEXT_SYS_ACL_SET_FILE(handle, clientPath,
			acltype, theacl);
err:
	TALLOC_FREE(clientPath);
out:
	return status;
}

/*
 * Success: return 0
 * Failure: set errno, return -1
 */
static int um_sys_acl_delete_def_file(vfs_handle_struct *handle,
		const char *path)
{
	int status;
	char *clientPath;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering um_sys_acl_delete_def_file\n"));
	if (!is_in_media_files(path))
	{
		status = SMB_VFS_NEXT_SYS_ACL_DELETE_DEF_FILE(handle,
				path);
		goto out;
	}

	clientPath = NULL;
	ctx = talloc_tos();

	if ((status = alloc_get_client_path(handle, ctx,
				path,
				&clientPath)))
	{
		goto err;
	}

	status = SMB_VFS_NEXT_SYS_ACL_DELETE_DEF_FILE(handle, clientPath);
err:
	TALLOC_FREE(clientPath);
out:
	return status;
}

/*
 * Success: return positive number
 * Failure: set errno, return -1
 * In this case, "name" is an attr name.
 */
static ssize_t um_getxattr(struct vfs_handle_struct *handle,
		const char *path,
		const char *name,
		void *value,
		size_t size)
{
	ssize_t ret;
	char *clientPath;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering um_getxattr\n"));
	if (!is_in_media_files(path))
	{
		ret = SMB_VFS_NEXT_GETXATTR(handle, path, name, value,
				size);
		goto out;
	}

	clientPath = NULL;
	ctx = talloc_tos();

	if (alloc_get_client_path(handle, ctx,
				path,
				&clientPath))
	{
		ret = -1;
		goto err;
	}

	ret = SMB_VFS_NEXT_GETXATTR(handle, clientPath, name, value, size);
err:
	TALLOC_FREE(clientPath);
out:
	return ret;
}

/*
 * Success: return positive number
 * Failure: set errno, return -1
 */
static ssize_t um_listxattr(struct vfs_handle_struct *handle,
		const char *path,
		char *list,
		size_t size)
{
	ssize_t ret;
	char *clientPath;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering um_listxattr\n"));
	if (!is_in_media_files(path))
	{
		ret = SMB_VFS_NEXT_LISTXATTR(handle, path, list, size);
		goto out;
	}

	clientPath = NULL;
	ctx = talloc_tos();

	if (alloc_get_client_path(handle, ctx,
				path,
				&clientPath))
	{
		ret = -1;
		goto err;
	}

	ret = SMB_VFS_NEXT_LISTXATTR(handle, clientPath, list, size);
err:
	TALLOC_FREE(clientPath);
out:
	return ret;
}

/*
 * Success: return 0
 * Failure: set errno, return -1
 * In this case, "name" is an attr name.
 */
static int um_removexattr(struct vfs_handle_struct *handle,
		const char *path,
		const char *name)
{
	int status;
	char *clientPath;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering um_removexattr\n"));
	if (!is_in_media_files(path))
	{
		status = SMB_VFS_NEXT_REMOVEXATTR(handle, path, name);
		goto out;
	}

	clientPath = NULL;
	ctx = talloc_tos();

	if ((status = alloc_get_client_path(handle, ctx,
				path,
				&clientPath)))
	{
		goto err;
	}

	status = SMB_VFS_NEXT_REMOVEXATTR(handle, clientPath, name);
err:
	TALLOC_FREE(clientPath);
out:
	return status;
}

/*
 * Success: return 0
 * Failure: set errno, return -1
 * In this case, "name" is an attr name.
 */
static int um_setxattr(struct vfs_handle_struct *handle,
		const char *path,
		const char *name,
		const void *value,
		size_t size,
		int flags)
{
	int status;
	char *clientPath;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering um_setxattr\n"));
	if (!is_in_media_files(path))
	{
		status = SMB_VFS_NEXT_SETXATTR(handle, path, name, value,
				size, flags);
		goto out;
	}

	clientPath = NULL;
	ctx = talloc_tos();

	if ((status = alloc_get_client_path(handle, ctx,
				path,
				&clientPath)))
	{
		goto err;
	}

	status = SMB_VFS_NEXT_SETXATTR(handle, clientPath, name, value,
			size, flags);
err:
	TALLOC_FREE(clientPath);
out:
	return status;
}

/*
 * Success: return true
 * Failure: set errno, return false
 */
static bool um_is_offline(struct vfs_handle_struct *handle,
		const struct smb_filename *fname,
		SMB_STRUCT_STAT *sbuf)
{
	// check if sbuf is modified further down the chain.
	bool ret;
	struct smb_filename *clientFname;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering um_is_offline\n"));
	if (!is_in_media_files(fname->base_name))
	{
		ret = SMB_VFS_NEXT_IS_OFFLINE(handle, fname, sbuf);
		goto out;
	}

	clientFname = NULL;
	ctx = talloc_tos();

	if(alloc_get_client_smb_fname(handle, ctx,
				fname,
				&clientFname))
	{
		ret = -1;
		goto err;
	}

	ret = SMB_VFS_NEXT_IS_OFFLINE(handle, clientFname, sbuf);
err:
	TALLOC_FREE(clientFname);
out:
	return ret;
}

/*
 * Success: return 0 (?)
 * Failure: set errno, return -1
 */
static int um_set_offline(struct vfs_handle_struct *handle,
		const struct smb_filename *fname)
{
	int status;
	struct smb_filename *clientFname;
	TALLOC_CTX *ctx;

	DEBUG(UM_INFO_DEBUG, ("Entering um_set_offline\n"));
	if (!is_in_media_files(fname->base_name))
	{
		status = SMB_VFS_NEXT_SET_OFFLINE(handle, fname);
		goto out;
	}

	clientFname = NULL;
	ctx = talloc_tos();

	if ((status = alloc_get_client_smb_fname(handle, ctx,
				fname,
				&clientFname)))
	{
		goto err;
	}

	status = SMB_VFS_NEXT_SET_OFFLINE(handle, clientFname);
err:
	TALLOC_FREE(clientFname);
out:
	return status;
}

/* VFS operations structure */

static struct vfs_fn_pointers vfs_um_fns = {
	/* Disk operations */

	.statvfs_fn = um_statvfs,

	/* Directory operations */

	.opendir_fn = um_opendir,
	.fdopendir_fn = um_fdopendir,
	.readdir_fn = um_readdir,
	.seekdir_fn = um_seekdir,
	.telldir_fn = um_telldir,
	.rewind_dir_fn = um_rewinddir,
	.mkdir_fn = um_mkdir,
	.rmdir_fn = um_rmdir,
	.closedir_fn = um_closedir,
	.init_search_op_fn = um_init_search_op,

	/* File operations */

	.open_fn = um_open,
	.create_file_fn = um_create_file,
	.rename_fn = um_rename,
	.stat_fn = um_stat,
	.lstat_fn = um_lstat,
	.fstat_fn = um_fstat,
	.unlink_fn = um_unlink,
	.chmod_fn = um_chmod,
	.chown_fn = um_chown,
	.lchown_fn = um_lchown,
	.chdir_fn = um_chdir,
	.ntimes_fn = um_ntimes,
	.symlink_fn = um_symlink,
	.readlink_fn = um_readlink,
	.link_fn = um_link,
	.mknod_fn = um_mknod,
	.realpath_fn = um_realpath,
	.chflags_fn = um_chflags,
	.streaminfo_fn = um_streaminfo,

	/* NT ACL operations. */

	.get_nt_acl_fn = um_get_nt_acl,

	/* POSIX ACL operations. */

	.chmod_acl_fn = um_chmod_acl,

	.sys_acl_get_file_fn = um_sys_acl_get_file,
	.sys_acl_set_file_fn = um_sys_acl_set_file,
	.sys_acl_delete_def_file_fn = um_sys_acl_delete_def_file,

	/* EA operations. */
	.getxattr_fn = um_getxattr,
	.listxattr_fn = um_listxattr,
	.removexattr_fn = um_removexattr,
	.setxattr_fn = um_setxattr,

	/* aio operations */

	/* offline operations */
	.is_offline_fn = um_is_offline,
	.set_offline_fn = um_set_offline
};

//NTSTATUS vfs_unityed_media_init(void);
//NTSTATUS vfs_unityed_media_init(void);
NTSTATUS samba_init_module(void);
NTSTATUS samba_init_module(void)
{
	NTSTATUS ret = smb_register_vfs(SMB_VFS_INTERFACE_VERSION,
				"unityed_media", &vfs_um_fns);
	if (!NT_STATUS_IS_OK(ret))
	{
		goto out;
	}

	vfs_um_debug_level = debug_add_class("unityed_media");

	if (vfs_um_debug_level == -1) {
		vfs_um_debug_level = DBGC_VFS;
		DEBUG(1, ("unityed_media_init: Couldn't register custom "
				"debugging class.\n"));
	} else {
		DEBUG(3, ("unityed_media_init: Debug class number of "
				"'unityed_media': %d\n",
				vfs_um_debug_level));
	}

out:
	return ret;
}
