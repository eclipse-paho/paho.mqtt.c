/*******************************************************************************
 * Copyright (c) 2009, 2023 IBM Corp.
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v2.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    https://www.eclipse.org/legal/epl-2.0/
 * and the Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Ian Craggs - initial API and implementation and/or initial documentation
 *    Ian Craggs - async client updates
 *    Ian Craggs - fix for bug 484496
 *    Ian Craggs - fix for issue 285
 *******************************************************************************/

/**
 * @file
 * \brief A file system based persistence implementation.
 *
 * A directory is specified when the MQTT client is created. When the persistence is then
 * opened (see ::Persistence_open), a sub-directory is made beneath the base for this
 * particular client ID and connection key. This allows one persistence base directory to
 * be shared by multiple clients.
 *
 */

#if !defined(NO_PERSISTENCE)

#include "OsWrapper.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#if defined(_WIN32) || defined(_WIN64)
	#include <direct.h>
	/* Windows doesn't have strtok_r, so remap it to strtok */
	#define strtok_r( A, B, C ) strtok( A, B )
	#define snprintf _snprintf
	int keysWin32(char *, char ***, int *);
	int clearWin32(char *);
	int containskeyWin32(char *, char *);
#else
	#include <sys/stat.h>
	#include <dirent.h>
	#include <unistd.h>
	int keysUnix(char *, char ***, int *);
	int clearUnix(char *);
	int containskeyUnix(char *, char *);
#endif

#include "MQTTClientPersistence.h"
#include "MQTTPersistenceDefault.h"
#include "StackTrace.h"
#include "Heap.h"

/** Create persistence directory for the client: context/clientID-serverURI.
 *  See ::Persistence_open
 */

int pstopen(void **handle, const char* clientID, const char* serverURI, void* context)
{
	int rc = 0;
	char *dataDir = context;
	char *clientDir;
	char *pToken = NULL;
	char *save_ptr = NULL;
	char *pCrtDirName = NULL;
	char *pTokDirName = NULL;
	char *perserverURI = NULL, *ptraux;
	size_t alloclen = 0;

	FUNC_ENTRY;
	/* Note that serverURI=address:port, but ":" not allowed in Windows directories */
	if ((perserverURI = malloc(strlen(serverURI) + 1)) == NULL)
	{
		rc = PAHO_MEMORY_ERROR;
		goto exit;
	}
	strcpy(perserverURI, serverURI);
	while ((ptraux = strstr(perserverURI, ":")) != NULL)
		*ptraux = '-' ;

	/* consider '/'  +  '-'  +  '\0' */
	alloclen = strlen(dataDir) + strlen(clientID) + strlen(perserverURI) + 3;
	clientDir = malloc(alloclen);
	if (!clientDir)
	{
		free(perserverURI);
		rc = PAHO_MEMORY_ERROR;
		goto exit;
	}
	if (snprintf(clientDir, alloclen, "%s/%s-%s", dataDir, clientID, perserverURI) >= alloclen)
	{
		free(clientDir);
		free(perserverURI);
		rc = MQTTCLIENT_PERSISTENCE_ERROR;
		goto exit;
	}

	/* create clientDir directory */

	/* pCrtDirName - holds the directory name we are currently trying to create.           */
	/*               This gets built up level by level untipwdl the full path name is created.*/
	/* pTokDirName - holds the directory name that gets used by strtok.         */
	if ((pCrtDirName = (char*)malloc(strlen(clientDir) + 1)) == NULL)
	{
		free(clientDir);
		free(perserverURI);
		rc = PAHO_MEMORY_ERROR;
		goto exit;
	}
	if ((pTokDirName = (char*)malloc( strlen(clientDir) + 1 )) == NULL)
	{
		free(pCrtDirName);
		free(clientDir);
		free(perserverURI);
		rc = PAHO_MEMORY_ERROR;
		goto exit;
	}
	strcpy( pTokDirName, clientDir );

	/* If first character is directory separator, make sure it's in the created directory name #285 */
	if (*pTokDirName == '/' || *pTokDirName == '\\')
	{
		*pCrtDirName = *pTokDirName;
		pToken = strtok_r( pTokDirName + 1, "\\/", &save_ptr );
		strcpy( pCrtDirName + 1, pToken );
	}
	else
	{
		pToken = strtok_r( pTokDirName, "\\/", &save_ptr );
		strcpy( pCrtDirName, pToken );
	}

	rc = pstmkdir( pCrtDirName );
	pToken = strtok_r( NULL, "\\/", &save_ptr );
	while ( (pToken != NULL) && (rc == 0) )
	{
		/* Append the next directory level and try to create it */
		strcat( pCrtDirName, "/" );
		strcat( pCrtDirName, pToken );
		rc = pstmkdir( pCrtDirName );
		pToken = strtok_r( NULL, "\\/", &save_ptr );
	}

	*handle = clientDir;

	free(pTokDirName);
	free(pCrtDirName);
	free(perserverURI);

exit:
	FUNC_EXIT_RC(rc);
	return rc;
}

/** Function to create a directory.
 * Returns 0 on success or if the directory already exists.
 */
int pstmkdir( char *pPathname )
{
	int rc = 0;

	FUNC_ENTRY;
#if defined(_WIN32) || defined(_WIN64)
	if ( _mkdir( pPathname ) != 0 )
	{
#else
	/* Create a directory with read, write and execute access for the owner and read access for the group */
#if !defined(_WRS_KERNEL)
	if ( mkdir( pPathname, S_IRWXU | S_IRGRP ) != 0 )
#else
	if ( mkdir( pPathname ) != 0 )
#endif /* !defined(_WRS_KERNEL) */
	{
#endif
		if ( errno != EEXIST )
			rc = MQTTCLIENT_PERSISTENCE_ERROR;
	}

	FUNC_EXIT_RC(rc);
	return rc;
}



/** Write wire message to the client persistence directory.
 *  See ::Persistence_put
 */
int pstput(void* handle, char* key, int bufcount, char* buffers[], int buflens[])
{
	int rc = 0;
	char *clientDir = handle;
	char *file;
	FILE *fp;
	size_t bytesWritten = 0,
	       bytesTotal = 0;
	int i;
	size_t alloclen = 0;

	FUNC_ENTRY;
	if (clientDir == NULL)
	{
		rc = MQTTCLIENT_PERSISTENCE_ERROR;
		goto exit;
	}

	/* consider '/' + '\0' */
	alloclen = strlen(clientDir) + strlen(key) + strlen(MESSAGE_FILENAME_EXTENSION) + 2;
	file = malloc(alloclen);
	if (!file)
	{
		rc = PAHO_MEMORY_ERROR;
		goto exit;
	}
	if (snprintf(file, alloclen, "%s/%s%s", clientDir, key, MESSAGE_FILENAME_EXTENSION) >= alloclen)
	{
		rc = MQTTCLIENT_PERSISTENCE_ERROR;
		goto free_exit;
	}

	fp = fopen(file, "wb");
	if ( fp != NULL )
	{
		for(i=0; i<bufcount; i++)
		{
			bytesTotal += buflens[i];
			bytesWritten += fwrite(buffers[i], sizeof(char), buflens[i], fp );
		}
		fclose(fp);
		fp = NULL;
	} else
		rc = MQTTCLIENT_PERSISTENCE_ERROR;

	if (bytesWritten != bytesTotal)
	{
		pstremove(handle, key);
		rc = MQTTCLIENT_PERSISTENCE_ERROR;
	}

free_exit:
	free(file);
exit:
	FUNC_EXIT_RC(rc);
	return rc;
};


/** Retrieve a wire message from the client persistence directory.
 *  See ::Persistence_get
 */
int pstget(void* handle, char* key, char** buffer, int* buflen)
{
	int rc = 0;
	FILE *fp = NULL;
	char *clientDir = handle;
	char *filename = NULL;
	char *buf = NULL;
	unsigned long fileLen = 0;
	unsigned long bytesRead = 0;
	size_t alloclen = 0;

	FUNC_ENTRY;
	if (clientDir == NULL)
	{
		rc = MQTTCLIENT_PERSISTENCE_ERROR;
		goto exit;
	}

	/* consider '/' + '\0' */
	alloclen = strlen(clientDir) + strlen(key) + strlen(MESSAGE_FILENAME_EXTENSION) + 2;
	filename = malloc(alloclen);
	if (!filename)
	{
		rc = PAHO_MEMORY_ERROR;
		goto exit;
	}
	if (snprintf(filename, alloclen, "%s/%s%s", clientDir, key, MESSAGE_FILENAME_EXTENSION) >= alloclen)
	{
		rc = MQTTCLIENT_PERSISTENCE_ERROR;
		free(filename);
		goto exit;
	}

	fp = fopen(filename, "rb");
	free(filename);
	if (fp != NULL)
	{
		fseek(fp, 0, SEEK_END);
		fileLen = ftell(fp);
		fseek(fp, 0, SEEK_SET);
		if ((buf = (char *)malloc(fileLen)) == NULL)
		{
			rc = PAHO_MEMORY_ERROR;
			goto exit;
		}
		bytesRead = (int)fread(buf, sizeof(char), fileLen, fp);
		*buffer = buf;
		*buflen = bytesRead;
		if ( bytesRead != fileLen )
			rc = MQTTCLIENT_PERSISTENCE_ERROR;
		fclose(fp);
	} else
		rc = MQTTCLIENT_PERSISTENCE_ERROR;

	/* the caller must free buf */
exit:
	FUNC_EXIT_RC(rc);
	return rc;
}



/** Delete a persisted message from the client persistence directory.
 *  See ::Persistence_remove
 */
int pstremove(void* handle, char* key)
{
	int rc = 0;
	char *clientDir = handle;
	char *file;
	size_t alloclen = 0;

	FUNC_ENTRY;
	if (clientDir == NULL)
	{
		rc = MQTTCLIENT_PERSISTENCE_ERROR;
		goto exit;
	}

	/* consider '/' + '\0' */
	/* consider '/' + '\0' */
	alloclen = strlen(clientDir) + strlen(key) + strlen(MESSAGE_FILENAME_EXTENSION) + 2;
	file = malloc(alloclen);
	if (!file)
	{
		rc = PAHO_MEMORY_ERROR;
		goto exit;
	}
	if (snprintf(file, alloclen, "%s/%s%s", clientDir, key, MESSAGE_FILENAME_EXTENSION) >= alloclen)
		rc = MQTTCLIENT_PERSISTENCE_ERROR;
	else
	{
#if defined(_WIN32) || defined(_WIN64)
		if ( _unlink(file) != 0 )
		{
#else
		if ( unlink(file) != 0 )
		{
#endif
			if ( errno != ENOENT )
				rc = MQTTCLIENT_PERSISTENCE_ERROR;
		}
	}

	free(file);
exit:
	FUNC_EXIT_RC(rc);
	return rc;
}


/** Delete client persistence directory (if empty).
 *  See ::Persistence_close
 */
int pstclose(void* handle)
{
	int rc = 0;
	char *clientDir = handle;

	FUNC_ENTRY;
	if (clientDir == NULL)
	{
		rc = MQTTCLIENT_PERSISTENCE_ERROR;
		goto exit;
	}

#if defined(_WIN32) || defined(_WIN64)
	if ( _rmdir(clientDir) != 0 )
	{
#else
	if ( rmdir(clientDir) != 0 )
	{
#endif
		if ( errno != ENOENT && errno != ENOTEMPTY )
			rc = MQTTCLIENT_PERSISTENCE_ERROR;
	}

	free(clientDir);

exit:
	FUNC_EXIT_RC(rc);
	return rc;
}


/** Returns whether if a wire message is persisted in the client persistence directory.
 * See ::Persistence_containskey
 */
int pstcontainskey(void *handle, char *key)
{
	int rc = 0;
	char *clientDir = handle;

	FUNC_ENTRY;
	if (clientDir == NULL)
	{
		rc = MQTTCLIENT_PERSISTENCE_ERROR;
		goto exit;
	}

#if defined(_WIN32) || defined(_WIN64)
	rc = containskeyWin32(clientDir, key);
#else
	rc = containskeyUnix(clientDir, key);
#endif

exit:
	FUNC_EXIT_RC(rc);
	return rc;
}


#if defined(_WIN32) || defined(_WIN64)
int containskeyWin32(char *dirname, char *key)
{
	int notFound = MQTTCLIENT_PERSISTENCE_ERROR;
	int fFinished = 0;
	char *filekey, *ptraux;
#if defined(_WIN32) || defined(_WIN64)
#define DIRSIZE MAX_PATH+1
#else
	const size_t DIRSIZE = MAX_PATH+1;
#endif
	char dir[DIRSIZE];
	WIN32_FIND_DATAA FileData;
	HANDLE hDir;

	FUNC_ENTRY;
	if (snprintf(dir, DIRSIZE, "%s/*", dirname) >= DIRSIZE)
		goto exit;

	hDir = FindFirstFileA(dir, &FileData);
	if (hDir != INVALID_HANDLE_VALUE)
	{
		while (!fFinished)
		{
			if (FileData.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE)
			{
				if ((filekey = malloc(strlen(FileData.cFileName) + 1)) == NULL)
				{
					notFound = PAHO_MEMORY_ERROR;
					goto exit;
				}
				strcpy(filekey, FileData.cFileName);
				ptraux = strstr(filekey, MESSAGE_FILENAME_EXTENSION);
				if ( ptraux != NULL )
					*ptraux = '\0' ;
				if(strcmp(filekey, key) == 0)
				{
					notFound = 0;
					fFinished = 1;
				}
				free(filekey);
			}
			if (!FindNextFileA(hDir, &FileData))
			{
				if (GetLastError() == ERROR_NO_MORE_FILES)
					fFinished = 1;
			}
		}
		FindClose(hDir);
	}
exit:
	FUNC_EXIT_RC(notFound);
	return notFound;
}
#else
int containskeyUnix(char *dirname, char *key)
{
	int notFound = MQTTCLIENT_PERSISTENCE_ERROR;
	char *filekey, *ptraux;
	DIR *dp = NULL;
	struct dirent *dir_entry;
	struct stat stat_info;

	FUNC_ENTRY;
	if((dp = opendir(dirname)) != NULL)
	{
		while((dir_entry = readdir(dp)) != NULL && notFound)
		{
			const size_t allocsize = strlen(dirname) + strlen(dir_entry->d_name) + 2;
			char* filename = malloc(allocsize);

			if (!filename)
			{
				notFound = PAHO_MEMORY_ERROR;
				goto exit;
			}
			if (snprintf(filename, allocsize, "%s/%s", dirname, dir_entry->d_name) >= allocsize)
			{
				free(filename);
				notFound = MQTTCLIENT_PERSISTENCE_ERROR;
				goto exit;
			}
			lstat(filename, &stat_info);
			free(filename);
			if(S_ISREG(stat_info.st_mode))
			{
				if ((filekey = malloc(strlen(dir_entry->d_name) + 1)) == NULL)
				{
					notFound = PAHO_MEMORY_ERROR;
					goto exit;
				}
				strcpy(filekey, dir_entry->d_name);
				ptraux = strstr(filekey, MESSAGE_FILENAME_EXTENSION);
				if ( ptraux != NULL )
					*ptraux = '\0' ;
				if(strcmp(filekey, key) == 0)
					notFound = 0;
				free(filekey);
			}
		}
	}

exit:
	if (dp)
		closedir(dp);
	FUNC_EXIT_RC(notFound);
	return notFound;
}
#endif


/** Delete all the persisted message in the client persistence directory.
 * See ::Persistence_clear
 */
int pstclear(void *handle)
{
	int rc = 0;
	char *clientDir = handle;

	FUNC_ENTRY;
	if (clientDir == NULL)
	{
		rc = MQTTCLIENT_PERSISTENCE_ERROR;
		goto exit;
	}

#if defined(_WIN32) || defined(_WIN64)
	rc = clearWin32(clientDir);
#else
	rc = clearUnix(clientDir);
#endif

exit:
	FUNC_EXIT_RC(rc);
	return rc;
}


#if defined(_WIN32) || defined(_WIN64)
int clearWin32(char *dirname)
{
	int rc = 0;
	int fFinished = 0;
	char *file;
	char dir[DIRSIZE];
	WIN32_FIND_DATAA FileData;
	HANDLE hDir;

	FUNC_ENTRY;
	if (snprintf(dir, DIRSIZE, "%s/*", dirname) >= DIRSIZE)
	{
		rc = MQTTCLIENT_PERSISTENCE_ERROR;
		goto exit;
	}

	hDir = FindFirstFileA(dir, &FileData);
	if (hDir != INVALID_HANDLE_VALUE)
	{
		while (!fFinished)
		{
			if (FileData.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE)
			{
				size_t allocsize = strlen(dirname) + strlen(FileData.cFileName) + 2;

				file = malloc(allocsize);
				if (!file)
				{
					rc = PAHO_MEMORY_ERROR;
					goto exit;
				}
				if (snprintf(file, allocsize, "%s/%s", dirname, FileData.cFileName) >= allocsize)
				{
					rc = MQTTCLIENT_PERSISTENCE_ERROR;
					free(file);
					goto exit;
				}
				rc = remove(file);
				free(file);
				if ( rc != 0 )
				{
					rc = MQTTCLIENT_PERSISTENCE_ERROR;
					break;
				}
			}
			if (!FindNextFileA(hDir, &FileData))
			{
				if (GetLastError() == ERROR_NO_MORE_FILES)
					fFinished = 1;
			}
		}
		FindClose(hDir);
	} else
		rc = MQTTCLIENT_PERSISTENCE_ERROR;

exit:
	FUNC_EXIT_RC(rc);
	return rc;
}
#else
int clearUnix(char *dirname)
{
	int rc = 0;
	DIR *dp;
	struct dirent *dir_entry;
	struct stat stat_info;

	FUNC_ENTRY;
	if((dp = opendir(dirname)) != NULL)
	{
		while((dir_entry = readdir(dp)) != NULL && rc == 0)
		{
			if (lstat(dir_entry->d_name, &stat_info) == 0 && S_ISREG(stat_info.st_mode))
			{
				if (remove(dir_entry->d_name) != 0 && errno != ENOENT)
					rc = MQTTCLIENT_PERSISTENCE_ERROR;
			}
		}
		closedir(dp);
	} else
		rc = MQTTCLIENT_PERSISTENCE_ERROR;

	FUNC_EXIT_RC(rc);
	return rc;
}
#endif


/** Returns the keys (file names w/o the extension) in the client persistence directory.
 *  See ::Persistence_keys
 */
int pstkeys(void *handle, char ***keys, int *nkeys)
{
	int rc = 0;
	char *clientDir = handle;

	FUNC_ENTRY;
	if (clientDir == NULL)
	{
		rc = MQTTCLIENT_PERSISTENCE_ERROR;
		goto exit;
	}

#if defined(_WIN32) || defined(_WIN64)
	rc = keysWin32(clientDir, keys, nkeys);
#else
	rc = keysUnix(clientDir, keys, nkeys);
#endif

exit:
	FUNC_EXIT_RC(rc);
	return rc;
}


#if defined(_WIN32) || defined(_WIN64)
int keysWin32(char *dirname, char ***keys, int *nkeys)
{
	int rc = 0;
	char **fkeys = NULL;
	int nfkeys = 0;
	char dir[DIRSIZE];
	WIN32_FIND_DATAA FileData;
	HANDLE hDir;
	int fFinished = 0;
	char *ptraux;
	int i;

	FUNC_ENTRY;
	if (snprintf(dir, DIRSIZE, "%s/*", dirname) >= DIRSIZE)
	{
		rc = MQTTCLIENT_PERSISTENCE_ERROR;
		goto exit;
	}

	/* get number of keys */
	hDir = FindFirstFileA(dir, &FileData);
	if (hDir != INVALID_HANDLE_VALUE)
	{
		while (!fFinished)
		{
			if (FileData.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE)
				nfkeys++;
			if (!FindNextFileA(hDir, &FileData))
			{
				if (GetLastError() == ERROR_NO_MORE_FILES)
					fFinished = 1;
			}
		}
		FindClose(hDir);
	} else
	{
		rc = MQTTCLIENT_PERSISTENCE_ERROR;
		goto exit;
	}

	if (nfkeys != 0)
	{
		if ((fkeys = (char **)malloc(nfkeys * sizeof(char *))) == NULL)
		{
			rc = PAHO_MEMORY_ERROR;
			goto exit;
		}
	}

	/* copy the keys */
	hDir = FindFirstFileA(dir, &FileData);
	if (hDir != INVALID_HANDLE_VALUE)
	{
		fFinished = 0;
		i = 0;
		while (!fFinished)
		{
			if (FileData.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE)
			{
				if ((fkeys[i] = malloc(strlen(FileData.cFileName) + 1)) == NULL)
				{
					rc = PAHO_MEMORY_ERROR;
					goto exit;
				}
				strcpy(fkeys[i], FileData.cFileName);
				ptraux = strstr(fkeys[i], MESSAGE_FILENAME_EXTENSION);
				if ( ptraux != NULL )
					*ptraux = '\0' ;
				i++;
			}
			if (!FindNextFileA(hDir, &FileData))
			{
				if (GetLastError() == ERROR_NO_MORE_FILES)
					fFinished = 1;
			}
		}
		FindClose(hDir);
	} else
	{
		rc = MQTTCLIENT_PERSISTENCE_ERROR;
		goto exit;
	}

	*nkeys = nfkeys;
	*keys = fkeys;
	/* the caller must free keys */

exit:
	FUNC_EXIT_RC(rc);
	return rc;
}
#else
int keysUnix(char *dirname, char ***keys, int *nkeys)
{
	int rc = 0;
	char **fkeys = NULL;
	int nfkeys = 0;
	char *ptraux;
	int i;
	DIR *dp = NULL;
	struct dirent *dir_entry;
	struct stat stat_info;

	FUNC_ENTRY;
	/* get number of keys */
	if((dp = opendir(dirname)) != NULL)
	{
		while((dir_entry = readdir(dp)) != NULL)
		{
			size_t allocsize = strlen(dirname)+strlen(dir_entry->d_name)+2;
			char* temp = malloc(allocsize);

			if (!temp)
			{
				rc = PAHO_MEMORY_ERROR;
				goto exit;
			}
			if (snprintf(temp, allocsize, "%s/%s", dirname, dir_entry->d_name) >= allocsize)
			{
				free(temp);
				rc = MQTTCLIENT_PERSISTENCE_ERROR;
				goto exit;
			}
			if (lstat(temp, &stat_info) == 0 && S_ISREG(stat_info.st_mode))
				nfkeys++;
			free(temp);
		}
		closedir(dp);
		dp = NULL;
	} else
	{
		rc = MQTTCLIENT_PERSISTENCE_ERROR;
		goto exit;
	}

	if (nfkeys != 0)
	{
		if ((fkeys = (char **)malloc(nfkeys * sizeof(char *))) == NULL)
		{
			rc = PAHO_MEMORY_ERROR;
			goto exit;
		}

		/* copy the keys */
		if((dp = opendir(dirname)) != NULL)
		{
			i = 0;
			while((dir_entry = readdir(dp)) != NULL)
			{
				size_t allocsize = strlen(dirname)+strlen(dir_entry->d_name)+2;
				char* temp = malloc(allocsize);

				if (!temp)
				{
					int n = 0;
					for (n = 0; n < i; n++)
						free(fkeys[n]);
					free(fkeys);
					rc = PAHO_MEMORY_ERROR;
					goto exit;
				}
				if (snprintf(temp, allocsize, "%s/%s", dirname, dir_entry->d_name) >= allocsize)
				{
					int n = 0;
					for (n = 0; n < i; n++)
						free(fkeys[n]);
					free(temp);
					free(fkeys);
					rc = MQTTCLIENT_PERSISTENCE_ERROR;
					goto exit;
				}
				if (lstat(temp, &stat_info) == 0 && S_ISREG(stat_info.st_mode))
				{
					if ((fkeys[i] = malloc(strlen(dir_entry->d_name) + 1)) == NULL)
					{
						int n = 0;
						for (n = 0; n < i; n++)
							free(fkeys[n]);
						free(temp);
						free(fkeys);
						rc = PAHO_MEMORY_ERROR;
						goto exit;
					}
					strcpy(fkeys[i], dir_entry->d_name);
					ptraux = strstr(fkeys[i], MESSAGE_FILENAME_EXTENSION);
					if ( ptraux != NULL )
						*ptraux = '\0' ;
					i++;
				}
				free(temp);
			}
		} else
		{
			rc = MQTTCLIENT_PERSISTENCE_ERROR;
			goto exit;
		}
	}

	*nkeys = nfkeys;
	*keys = fkeys;
	/* the caller must free keys */

exit:
	if (dp)
		closedir(dp);
	FUNC_EXIT_RC(rc);
	return rc;
}
#endif



#if defined(UNIT_TESTS)
int main (int argc, char *argv[])
{
#define MSTEM "m-"
#define NMSGS 10
#define NBUFS 4
#define NDEL 2
#define RC !rc ? "(Success)" : "(Failed) "

	int rc;
	char *handle;
	char *perdir = ".";
	const char *clientID = "TheUTClient";
	const char *serverURI = "127.0.0.1:1883";

	char *stem = MSTEM;
	int msgId, i;
	int nm[NDEL] = {5 , 8};  /* msgIds to get and remove */

	char *key;
	char **keys;
	int nkeys;
	char *buffer, *buff;
	int buflen;

	int nbufs = NBUFS;
	char *bufs[NBUFS] = {"m0", "mm1", "mmm2" , "mmmm3"};  /* message content */
	int buflens[NBUFS];
	for(i=0;i<nbufs;i++)
		buflens[i]=strlen(bufs[i]);

	/* open */
	/* printf("Persistence directory : %s\n", perdir); */
	rc = pstopen((void**)&handle, clientID, serverURI, perdir);
	printf("%s Persistence directory for client %s : %s\n", RC, clientID, handle);

	/* put */
	for(msgId=0;msgId<NMSGS;msgId++)
	{
		key = malloc(PERSISTENCE_MAX_KEY_LENGTH + 1);
		sprintf(key, "%s%d", stem, msgId);
		rc = pstput(handle, key, nbufs, bufs, buflens);
		printf("%s Adding message %s\n", RC, key);
		free(key);
	}

	/* keys ,ie, list keys added */
	rc = pstkeys(handle, &keys, &nkeys);
	printf("%s Found %d messages persisted in %s\n", RC, nkeys, handle);
	for(i=0;i<nkeys;i++)
		printf("%13s\n", keys[i]);

	if (keys !=NULL)
		free(keys);

	/* containskey */
	for(i=0;i<NDEL;i++)
	{
		key = malloc(PERSISTENCE_MAX_KEY_LENGTH + 1);
		sprintf(key, "%s%d", stem, nm[i]);
		rc = pstcontainskey(handle, key);
		printf("%s Message %s is persisted ?\n", RC, key);
		free(key);
	}

	/* get && remove*/
	for(i=0;i<NDEL;i++)
	{
		key = malloc(PERSISTENCE_MAX_KEY_LENGTH + 1);
		sprintf(key, "%s%d", stem, nm[i]);
		rc = pstget(handle, key, &buffer, &buflen);
		buff = malloc(buflen+1);
		memcpy(buff, buffer, buflen);
		buff[buflen] = '\0';
		printf("%s Retrieving message %s : %s\n", RC, key, buff);
		rc = pstremove(handle, key);
		printf("%s Removing message %s\n", RC, key);
		free(key);
		free(buff);
		free(buffer);
	}

	/* containskey */
	for(i=0;i<NDEL;i++)
	{
		key = malloc(PERSISTENCE_MAX_KEY_LENGTH + 1);
		sprintf(key, "%s%d", stem, nm[i]);
		rc = pstcontainskey(handle, key);
		printf("%s Message %s is persisted ?\n", RC, key);
		free(key);
	}

	/* keys ,ie, list keys added */
	rc = pstkeys(handle, &keys, &nkeys);
	printf("%s Found %d messages persisted in %s\n", RC, nkeys, handle);
	for(i=0;i<nkeys;i++)
		printf("%13s\n", keys[i]);

	if (keys != NULL)
		free(keys);


	/* close -> it will fail, since client persistence directory is not empty */
	rc = pstclose(&handle);
	printf("%s Closing client persistence directory for client %s\n", RC, clientID);

	/* clear */
	rc = pstclear(handle);
	printf("%s Deleting all persisted messages in %s\n", RC, handle);

	/* keys ,ie, list keys added */
	rc = pstkeys(handle, &keys, &nkeys);
	printf("%s Found %d messages persisted in %s\n", RC, nkeys, handle);
	for(i=0;i<nkeys;i++)
		printf("%13s\n", keys[i]);

	if ( keys != NULL )
		free(keys);

	/* close */
	rc = pstclose(&handle);
	printf("%s Closing client persistence directory for client %s\n", RC, clientID);
}
#endif


#endif /* NO_PERSISTENCE */
