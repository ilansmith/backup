#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#define DEFAULT_EDITOR "vim"
#define DEFAULT_DIFFPROG "diff"

#define ENV_HOME "HOME"
#define ENV_SHELL "SHELL"
#define ENV_EDITOR "EDITOR"
#define ENV_DIFFPROG "DIFFPROG"

#define STR_YES "yes"
#define STR_NO "no"

#define COLOR_HIGHLIGHT "\033[1m"
#define COLOR_UNDERLINE "\033[4m"
#define COLOR_ERROR "\033[1;31m" /* bright red */
#define COLOR_RESET "\033[00;00m"

#define TEXT_HIGHLIGHT(_str_) COLOR_HIGHLIGHT _str_ COLOR_RESET
#define TEXT_UNDERLINE(_str_) COLOR_UNDERLINE _str_ COLOR_RESET
#define TEXT_ERROR(_str_) COLOR_ERROR _str_ COLOR_RESET

#define MIN(_x_, _y_) ((_x_) < (_y_) ? (_x_) : (_y_))
#define MAX(_x_, _y_) ((_x_) < (_y_) ? (_y_) : (_x_))
#define MAX_PATH_LEN 256

#define CHAR_SP 0x20
#define CHAR_TAB 0x9 
#define CHAR_NL 0xA
#define CHAR_HS 0X23

#define IS_NEWLINE(_x_) ((_x_) == CHAR_NL)
#define IS_HASH(_x_) ((_x_) == CHAR_HS)
#define IS_WHITE_SPACE(_x_) (((_x_) == CHAR_SP) || ((_x_)== CHAR_TAB))
#define IS_WHITE_PREFIX(_x_) (!(_x_) || IS_HASH(_x_) || IS_NEWLINE(_x_))

/* print an error message */
#if defined(DEBUG)
#define ERROR(_fmt_, ...) \
    do \
    { \
        char errPrefix[256]; \
        snprintf(errPrefix, sizeof(errPrefix), "%s:%d, %s(): ", __FILE__, \
            __LINE__, __FUNCTION__); \
        PrintError(errPrefix, "", _fmt_, ##__VA_ARGS__); \
    } while (0)
#else
#define ERROR(_fmt_, ...) \
    do \
    { \
        PrintError("", "", _fmt_, ##__VA_ARGS__); \
    } while (0)
#endif

/* verify an external application used by backup exists. If it doesn't, abort */
#define TEST_EXTERNAL_APP_EXISTS(_app_, ...) \
    do \
    { \
        if (SysExec("which " _app_ " > /dev/null", ##__VA_ARGS__)) \
        { \
            PrintError("Can't find: ", "Aborting...", _app_, ##__VA_ARGS__); \
            exit(1); \
        } \
    } while (0)

#define IS_YES_NO_ANSWER(_ans_) \
    (((MAX(strlen(_ans_), strlen(STR_YES)) == strlen(STR_YES)) && \
    (!strncasecmp(_ans_, STR_YES, MIN(strlen(_ans_), strlen(STR_YES))))) || \
    ((MAX(strlen(_ans_), strlen(STR_NO)) == strlen(STR_NO)) && \
    !strncasecmp(_ans_, STR_NO, MIN(strlen(_ans_), strlen(STR_NO)))))

typedef enum 
{
    BACKUP_ERR_BACKUP = 1<<0,
    BACKUP_ERR_EDIT = 1<<1,
    BACKUP_ERR_VERSION = 1<<2,
    BACKUP_ERR_HELP = 1<<3,
    BACKUP_ERR_ARGS = 1<<4,
    BACKUP_ERR_NOTEXIST = 1<<5,
} backupErr_t;

typedef struct path_t 
{
    struct path_t *pNext;
    char *pStr;
} path_t;

typedef int (*fileProcessingHandler_t)(int isExist, char *pPath, void **ppData);

static char *gpHomeDir;
static char gBackupTarGz[MAX_PATH_LEN];
static char gWorkingDir[MAX_PATH_LEN];
static char gBackupDir[MAX_PATH_LEN];
static char gBackupConf[MAX_PATH_LEN];
static int gSupressConfCleanup;

/* common format for printing an error */
static int PrintError(char *pErrPrefix, char *pErrSuffix, char *pFmt, ...)
{
#define MAX_ERR_LN 256
    va_list ap;
    char errStr[MAX_ERR_LN];

    memset(errStr, 0, MAX_ERR_LN);
    va_start(ap, pFmt);
    vsnprintf(errStr, MAX_ERR_LN, pFmt, ap);
    va_end(ap);

    return fprintf(stderr, "%s" TEXT_ERROR("%s") "%s%s\n", pErrPrefix, errStr, 
        *pErrSuffix ? ". " : "", pErrSuffix);
}

/* open a file and set its user id and group id */
static FILE *OpenId(char *pFileName, uid_t uid, gid_t gid)
{
    FILE *pFile;

    if (!(pFile = fopen(pFileName, "w+")))
        return NULL;

    chown(pFileName, uid, gid);
    return pFile;
}

static path_t *PathAlloc(char *pStr)
{
    path_t *pPath;
    int len;

    if (!(pPath = (path_t*)calloc(1, sizeof(path_t))))
        goto Error;

    len = strlen(pStr) + 1;
    if (!(pPath->pStr = (char*)calloc(len, sizeof(char))))
        goto Error;

    snprintf(pPath->pStr, len, "%s", pStr);
    return pPath;

Error:
    if (pPath)
        free(pPath);

    return NULL;
}

static void PathFree(path_t *pPath)
{
    if (!pPath)
        return;

    if (pPath->pStr)
        free(pPath->pStr);
    free(pPath);
}

/* removes a newline if it comes as the last non zero charcater in pStr
   return: 0 - if the string was not modified
           1 - if the string was modified */
static int RemoveNewline(char *pStr)
{
    char *pEol = pStr + strlen(pStr) - 1;

    if (*pEol != '\n')
        return 0;

    *pEol = 0;
    return 1;
}

/* adds a newline at the end of pStr if a the resulting string will still be 
   null terminated.
   return: 0 - if the string was not modified
           1 - if the string was modified */
static int AddNewline(char *pStr)
{
    char *pEol = pStr + strlen(pStr);

    if (*(pEol + 1))
        return 0;

    *pEol = '\n';
    return 1;
}

/* test if a line in the conf file is skippable */
static int IsWhiteline(char *pLine, int len)
{
    int i, isWl;

    for (i = 0; i < len && !(isWl = IS_WHITE_PREFIX(pLine[i])) && 
        IS_WHITE_SPACE(pLine[i]); i++);

    return i == len ? 1 : isWl;
}

/* find the first non white space character in a path buffer */
static char *DelLeadingWhite(char *pPath, int len)
{
    char *pStart = pPath;

    while (pPath && pPath - pStart < len && IS_WHITE_SPACE(*pPath))
        pPath++;
    return pPath;
}

static int IsDelObsoleteEntry(char *pPath)
{
#define INPUT_SZ 5

    char ans[INPUT_SZ];
    int ret, isFirst = 1;

    printf(TEXT_HIGHLIGHT("%s") " does not exist...\n", pPath);
    printf("do you want to remove it from the configuration file? [Y/n] ");

    /* repeat while answer provided by the user is not a case insensative 
       substring of 'yes' or 'no' */
    do
    {
        int isNewlineFound;

        if (!isFirst)
            printf("Please answer Y[es] or N[o]: ");
        else
            isFirst = 0;

        /* get user's answer */
        bzero(ans, INPUT_SZ);
        fgets(ans, INPUT_SZ, stdin);
        isNewlineFound = RemoveNewline(ans);

        /* if a newline was not encountered,
           it must be found and removed from stdin */
        if (!isNewlineFound)
            while (!IS_NEWLINE(fgetc(stdin)));
    } while (!IS_YES_NO_ANSWER(ans));

    switch (*ans)
    {
        case 0: /* a newline was replaced by the null terminator */
        case 'y':
        case 'Y':
            ret = 1;
            break;
        case 'n':
        case 'N':
            ret = 0;
            break;
        default:
            ERROR("unreachable switch case");
            exit(1);
            break;
    }
    return ret;
}

/* copy content from one file to another */
static int cpFile(FILE *pTo, FILE *pFrom)
{
    char line[MAX_PATH_LEN];

    bzero(line, MAX_PATH_LEN);
    while (fgets(line, MAX_PATH_LEN, pFrom))
    {
        fputs(line, pTo);
        bzero(line, MAX_PATH_LEN);
    }

    return fseek(pFrom, 0, SEEK_SET) || fseek(pTo, 0, SEEK_SET);
}

/* configuration file cleanup callback function */
static int HandlerConfCleanup(int isExist, char *pPath, void **ppData)
{
    FILE *pCnf = *((FILE**)ppData);
    char *pPathPtr = NULL;
    int isWl = IsWhiteline(pPath, MAX_PATH_LEN);

    pPathPtr = DelLeadingWhite(pPath, MAX_PATH_LEN);

    if (!isWl && !isExist && IsDelObsoleteEntry(pPathPtr))
        return 0; /* ignore this path, don't insert it in the new conf file */

    AddNewline(pPath);
    fputs(pPath, pCnf);
    return 0;
}

/* path data structure creation callback function */
static int HandlerPathsCreate(int isExist, char *pPath, void **ppData)
{
    path_t *pNew;

    if (!isExist)
        return 0;

    if (!(pNew = PathAlloc(DelLeadingWhite(pPath, MAX_PATH_LEN))))
        return -1;

    pNew->pNext = *((path_t**)ppData);
    *ppData = pNew;

    return 0;
}

/* generic function for processing configuration file lines (paths) */
static int FileProcessGeneric(FILE *pFile, fileProcessingHandler_t handler, 
    void **ppData)
{
    char path[MAX_PATH_LEN];

    bzero(path, MAX_PATH_LEN);
    while (fgets(path, MAX_PATH_LEN, pFile))
    {
        char *pPathPtr;
        int isExist;
        struct stat st;

        /* line is not whitespaces only or a comment */
        pPathPtr = DelLeadingWhite(path, MAX_PATH_LEN);
        RemoveNewline(pPathPtr);
        if (!(isExist = !stat(pPathPtr, &st)) && errno != ENOENT)
        {
            ERROR("stat(%s, &st), continuing...", pPathPtr);
            bzero(path, MAX_PATH_LEN);
            continue;
        }

        if (handler(isExist, path, ppData))
            return -1;
        bzero(path, MAX_PATH_LEN);
    }
    return 0;
}

/* allow user to remove stale paths from the configuration file */
static int ConfCleanup(void)
{
    char backupConfBck[MAX_PATH_LEN];
    FILE *pCnf, *pTmp, *pBck;
    struct stat st;

    if (gSupressConfCleanup)
        return 0;

    /* open pCnf FILE */
    if (!(pCnf = fopen(gBackupConf, "r+")))
    {
        ERROR("fopen(%s, \"r+\")", gBackupConf);
        return -1;
    }
    /* open pTmp FILE */
    if (!(pTmp = tmpfile()))
    {
        ERROR("tmpfile()");
        fclose(pCnf);
        return -1;
    }

    if (stat(gBackupConf, &st) == -1)
    {
        ERROR("stat(%s)", gBackupConf);
        fclose(pCnf);
        fclose(pTmp);
        return -1;
    }

    /* open bck FILE */
    snprintf(backupConfBck, MAX_PATH_LEN, "%s.bck", gBackupConf);
    if (!(pBck = OpenId(backupConfBck, st.st_uid, st.st_gid)))
    {
        ERROR("OpenId(%s, %d, %d)", backupConfBck, st.st_uid, st.st_gid);
        fclose(pCnf);
        fclose(pTmp);
        return -1;
    }

    /* copy conf file to backup file in case of program termination while conf
       file is being manipulated */
    cpFile(pBck, pCnf);
    fclose(pBck);

    /* copy conf file to pTmp file and create a new conf file */
    cpFile(pTmp, pCnf);
    fclose(pCnf);
    if (remove(gBackupConf))
    {
        ERROR("remove(%s)", gBackupConf);
        fclose(pTmp);
        return -1;
    }
    if (!(pCnf = OpenId(gBackupConf, st.st_uid, st.st_gid)))
    {
        ERROR("OpenId(%s, %d, %d)", gBackupConf, st.st_uid, st.st_gid);
        fclose(pTmp);
        return -1;
    }
    FileProcessGeneric(pTmp, HandlerConfCleanup, (void **)&pCnf);
    fclose(pTmp);
    fclose(pCnf);
    if (remove(backupConfBck))
        ERROR("remove(%s)", backupConfBck);

    return 0;
}

/* build a paths linked list */
static int PathsCreate(path_t **ppCpPaths)
{
    int ret;
    FILE *pCnf;

    /* open pCnf FILE */
    if (!(pCnf = fopen(gBackupConf, "r+")))
    {
        ERROR("fopen(%s, \"r+\")", gBackupConf);
        return -1;
    }
    ret = FileProcessGeneric(pCnf, HandlerPathsCreate, (void**)ppCpPaths);
    fclose(pCnf);

    return ret;
}

/* system execution function */
static int SysExec(char *pFormat, ...)
{
    int status = 0;

    if (!fork()) /* child process */
    {
        char command[MAX_PATH_LEN];
        va_list ap;
        int ret;
        char *args[4];

        bzero(command, MAX_PATH_LEN);
        va_start(ap, pFormat);
        ret = vsnprintf(command, MAX_PATH_LEN - 1, pFormat, ap);
        va_end(ap);

        if (ret < 0)
           exit(1);

        args[0] = getenv(ENV_SHELL);
        args[1] = "-c";
        args[2] = command;
        args[3] = NULL;
        execvp(args[0], args);
    }

    wait(&status);

    /* fail if the child process did not exit gracefully */
    return WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -1;
}

/* initialize global variables */
static int Init(void)
{
    if (!(gpHomeDir = getenv(ENV_HOME)))
    {
        ERROR("getenv()");
        return -1;
    }
    if (!getcwd(gWorkingDir, MAX_PATH_LEN))
    {
        ERROR("getcwd()");
        return -1;
    }
    if ((snprintf(gBackupTarGz, MAX_PATH_LEN, "%s/backup.tar.gz", 
        gWorkingDir)) < 0)
    {
        ERROR("snprintf()");
        return -1;
    }

    if ((snprintf(gBackupConf, MAX_PATH_LEN, "%s/.backup.conf", gpHomeDir) < 0))
        return -1;

    return 0;
}

/* create temporary backup dir */
static int CreateBackupDir(void)
{
#define BACKUP_DIR "backup"

    int ret, i = 0;
    char *pTmp;

    bzero(gBackupDir, MAX_PATH_LEN);
    pTmp = gBackupDir;
    /* add leading underscores if the backup directory allready exists */
    do
    {
        snprintf(&(pTmp[i]), MAX_PATH_LEN - (i + i), BACKUP_DIR);
        if ((ret = mkdir(gBackupDir, 0777)) == -1)
        {
            if (errno != EEXIST)
            {
                ERROR("mkdir()");
                return -1;
            }

            pTmp[i] = '_';
        }
        i++;
    } while (ret && (i < (MAX_PATH_LEN - strlen(gBackupConf) - 1)));

    return ret;
}

/* copy configuration file locations to backup directory */
static int CopyToBackupDir(void)
{
    path_t *cpPaths = NULL;

    ConfCleanup(); /* XXX handle errors */
    PathsCreate(&cpPaths); /* XXX handle errors */

    /* copy backup destinations to gBackupDir */
    printf("backing up...\n");
    while (cpPaths)
    {
        path_t *pTmp;

        if (SysExec("cp -r --parents %s %s", cpPaths->pStr, gBackupDir))
            ERROR("cp -r --parents %s %s", cpPaths->pStr, gBackupDir);
        pTmp = cpPaths;
        cpPaths = cpPaths->pNext;
        PathFree(pTmp);
    }

    return 0;
}

/* make a *.gar.gz of the temporary backup directory */
static int MakeTarGz(void)
{
    return SysExec("cd %s && tar czf %s .", gBackupDir, gBackupTarGz) ? 
        -1 : 0;
}

/* remove the backup directory */
static int RemoveBackupDir(void)
{
    if (SysExec("rm -rf %s", gBackupDir))
        return -1;
    printf("done: " TEXT_HIGHLIGHT("%s") "\n", gBackupTarGz);
    return 0;
}

/* test if a configuration file stated explicitly by the user exists */
static int IsOptionalBackupConfExit(char *pFileName)
{
    struct stat st;

    snprintf(gBackupConf, MAX_PATH_LEN, "%s", pFileName);
    return stat(gBackupConf, &st) ? 0 : 1;
}

/* parse user arguments */
static int GetArgs(int argc, char *argv[])
{
#define OPTSTRING "b::evhf"

    unsigned int ret = 0;
    char opt;

    while ((opt = (char)getopt(argc, argv, OPTSTRING)) != -1)
    {
        switch (opt)
        {
            case 'b': /* do backup */

                /* conflicting arguments:
                   - multiple do backup
                   - do edit
                   - get help
                   - get version */
                if (ret & (BACKUP_ERR_BACKUP | BACKUP_ERR_EDIT | 
                    BACKUP_ERR_HELP | BACKUP_ERR_VERSION))
                {
                    return BACKUP_ERR_ARGS;
                }
                if (optarg && !IsOptionalBackupConfExit(optarg))
                    return BACKUP_ERR_NOTEXIST;
                ret |= BACKUP_ERR_BACKUP;
                break;

            case 'e': /* do edit */
                /* conflicting arguments:
                   - do backup
                   - multiple do edit
                   - get help
                   - get version */
                if (ret & (BACKUP_ERR_BACKUP | BACKUP_ERR_EDIT | 
                    BACKUP_ERR_HELP | BACKUP_ERR_VERSION))
                {
                    return BACKUP_ERR_ARGS;
                }
                ret |= BACKUP_ERR_EDIT;
                break;

            case 'v': /* get version */
                /* conflicting arguments:
                   - do backup
                   - do edit
                   - get help
                   - multiple get version */
                if (ret & (BACKUP_ERR_BACKUP | BACKUP_ERR_EDIT | 
                    BACKUP_ERR_HELP | BACKUP_ERR_VERSION))
                {
                    return BACKUP_ERR_ARGS;
                }
                ret |= BACKUP_ERR_VERSION;
                break;

            case 'h': /* get help */
                /* conflicting arguments:
                   - do backup
                   - do edit
                   - multiple get help
                   - get version */
                if (ret & (BACKUP_ERR_BACKUP | BACKUP_ERR_EDIT | 
                    BACKUP_ERR_HELP | BACKUP_ERR_VERSION))
                {
                    return BACKUP_ERR_ARGS;
                }
                ret |= BACKUP_ERR_HELP;
                break;

            case 'f': /* force no configuration file cleanup */
                /* conflicting arguments:
                   - multiple force configuration cleanup
                   - get help
                   - get version */
                if (gSupressConfCleanup || ret & (BACKUP_ERR_HELP | 
                    BACKUP_ERR_VERSION))
                {
                    return BACKUP_ERR_ARGS;
                }
                gSupressConfCleanup = 1;
                break;
            default:
                return BACKUP_ERR_ARGS;
                break;
        }
    }
    return ret;
}

/* trim possible parameters off a command line, return the command name only */
static char *CmdLineTrim(char *pStr, char *pBuffer, int len)
{
    snprintf(pBuffer, len, "%s", pStr);
    strtok(pBuffer, " ");
    return pBuffer;
}

/* trim a full path down to the file / bottom directory name */
static char *AppNameGet(char *pPath)
{
    char *pAppName;

    for (pAppName = pPath + strlen(pPath) - 1; 
        pAppName >= pPath && *pAppName != '/'; pAppName--);

    return ++pAppName;
}

/* backup the locations stated in the configuration file */
static int Backup(void)
{
    TEST_EXTERNAL_APP_EXISTS("tar");

    return CreateBackupDir() || CopyToBackupDir() || MakeTarGz() || 
        RemoveBackupDir() ? -1 : 0;
}

/* edit the locations stated in the configuration file */
static int Edit(void)
{
    char *pEditor = DEFAULT_EDITOR;
    char *pDiffProg = DEFAULT_DIFFPROG;
    char *pEnvVar;
    char backupConfOld[MAX_PATH_LEN];
    char externalAppName[MAX_PATH_LEN];
    FILE *pOld, *pCnf;
    struct stat st;
    int isNew;

    bzero(backupConfOld, MAX_PATH_LEN);
    snprintf(backupConfOld, MAX_PATH_LEN, "%s.old", gBackupConf);

    isNew = stat(gBackupConf, &st) == -1 ? 1 : 0;
    if (!(pCnf = fopen(gBackupConf, "a+")))
    {
        ERROR("fopen(%s, \"a+\")", gBackupConf);
        return -1;
    }
    if (!(pOld = fopen(backupConfOld, "w+")))
    {
        ERROR("fopen(%s, \"w+\")", backupConfOld);
        fclose(pCnf);
        return -1;
    }

    cpFile(pOld, pCnf);
    fclose(pOld);
    fclose(pCnf);

    if ((pEnvVar = getenv(ENV_EDITOR)))
        pEditor = pEnvVar;
    if ((pEnvVar = getenv(ENV_DIFFPROG)))
        pDiffProg = pEnvVar;

    TEST_EXTERNAL_APP_EXISTS("%s", CmdLineTrim(pEditor, externalAppName, 
        MAX_PATH_LEN));
    TEST_EXTERNAL_APP_EXISTS("%s", CmdLineTrim(pDiffProg, externalAppName,
        MAX_PATH_LEN));

    SysExec("%s %s", pEditor, gBackupConf);
    if (!isNew)
    {
        SysExec("if [ -n \"`diff %s %s`\" ]; then %s %s %s; else echo "
            "\"%s has not been modified\"; fi", backupConfOld, gBackupConf, 
            pDiffProg, backupConfOld, gBackupConf, gBackupConf);
    }
    if (remove(backupConfOld))
        ERROR("remove(%s)", backupConfOld);
    ConfCleanup();

    return 0;
}

/* print application version */
static void Version(void)
{
#define VER_LENGTH 32
    char ver[VER_LENGTH];

#ifdef VERSION
    snprintf(ver, VER_LENGTH, TEXT_HIGHLIGHT("%.2f"), VERSION);
#else
    snprintf(ver, VER_LENGTH, "no data is available");
#endif

    printf("backup version: %s\n", ver);
}

/* print usage message */
static void Usage(char *pAppPath)
{
    char *pAppName = AppNameGet(pAppPath);

    printf(
        "usage:\n"
        "          " TEXT_HIGHLIGHT("%s < -e | -b [ conf_file ] > [ -f ]") "\n"
        "          " TEXT_HIGHLIGHT("%s < -v | -h >") "\n"
        "   where\n"
        "   " TEXT_HIGHLIGHT("-e") "  Edit the configuration file.\n"
        "       Enter the full paths to directories or files you "
        "wish to backup.\n"
        "       A line can be commented out by using the " 
        TEXT_HIGHLIGHT("#") " character.\n"
        "       Set the environment variable " TEXT_UNDERLINE("EDITOR") " to "
        "use an editor of your choice.\n"
        "       If " TEXT_UNDERLINE("EDITOR") " is not set, " 
        TEXT_HIGHLIGHT("%s") " will use " TEXT_HIGHLIGHT("%s") " for editing "
        "the configuration\n"
        "       file.\n"
        "       Once an existing configuration file is modified, "
        TEXT_HIGHLIGHT("%s") " displays a diff\n"
        "       between the previous and the current versions.\n"
        "       Set the environment variable " TEXT_UNDERLINE("DIFFPROG") " to "
        "use a diff program of your\n"
        "       choice. If " TEXT_UNDERLINE("DIFFPROG") " is not set, "
        TEXT_HIGHLIGHT("%s") " will use " TEXT_HIGHLIGHT("%s") ".\n"
        "   " TEXT_HIGHLIGHT("-b") "  Backup the files and directories "
        "mentioned in the configuration file.\n"
        "       " TEXT_HIGHLIGHT("%s") " uses the default configuration file "
        "(%s)\n"
        "       unless a " TEXT_HIGHLIGHT("conf_file") " is stated "
        "specifically.\n"
        "       The gzipped tarball " TEXT_HIGHLIGHT("%s.tar.gz") " containing "
        "the backed up files will be\n"
        "       placed in the working directory.\n"
        "   " TEXT_HIGHLIGHT("-f") "  Do not prompt to clean configuration "
        "file of redundant paths.\n"
        "   " TEXT_HIGHLIGHT("-v") "  Display " 
        TEXT_HIGHLIGHT("%s") " version.\n"
        "   " TEXT_HIGHLIGHT("-h") "  Print this message and exit.\n"
        "\n"
        "IAS, October 2004\n", pAppName, pAppName, pAppName, DEFAULT_EDITOR, 
         pAppName, pAppName, DEFAULT_DIFFPROG, pAppName, gBackupConf,
         pAppName, pAppName); 
}

static int ExecRootOnly(int (*pFunc)(void))
{
    if (getuid())
    {
        ERROR("Permission denied (must run as root)");
        return -1;
    }

    /* execute as root */
    return pFunc();
}

int main(int argc, char *argv[])
{
    int ret = 0;

    Init();

    switch (GetArgs(argc, argv))
    {
        case BACKUP_ERR_BACKUP:
            ret = ExecRootOnly(Backup);
            break;
        case BACKUP_ERR_EDIT:
            ret = ExecRootOnly(Edit);
            break;
        case BACKUP_ERR_VERSION:
            Version();
            break;
        case BACKUP_ERR_HELP:
            Usage(argv[0]);
            break;
        case BACKUP_ERR_NOTEXIST:
            fprintf(stderr, "file '%s' does not exit\n", gBackupConf);
            break;
        case BACKUP_ERR_ARGS: /* fall through */
        default:
            fprintf(stderr, "try '%s -h' for more information\n",
               AppNameGet(argv[0]));
            break;
    }

    return ret;
}

