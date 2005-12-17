#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#define ENV_HOME "HOME"
#define ENV_SHELL "SHELL"
#define ENV_EDITOR "EDITOR"
#define ENV_DIFFPROG "DIFFPROG"

#define ACT_BACKUP 1<<0
#define ACT_EDIT 1<<1
#define	ACT_VERSION 1<<2
#define ACT_HELP 1<<3
#define ACT_ERROR 1<<4
#define ACT_EXIT 1<<5
#define ACT_FORCE 1<<6

#define FMT_HIGHLIGHT "\033[1;38m"
#define FMT_UNDERLINE "\033[4;38m"
#define FMT_RESET "\033[00;00;00m"

#define MIN(X, Y) ((X) < (Y) ? (X) : (Y))
#define MAX(X, Y) ((X) < (Y) ? (Y) : (X))
#define MAX_PATH_LN 256

#define CHAR_SP 0x20
#define CHAR_TAB 0x9 
#define CHAR_NL 0xA
#define CHAR_HS 0X23
#define NEWLINE(X) ((X) == CHAR_NL)
#define HASH(X) ((X) == CHAR_HS)
#define WHITESPACE(X) (((X) == CHAR_SP) || ((X)== CHAR_TAB))
#define WHITELINE(X) (NEWLINE(X) || HASH(X))

static char *home_dir;
static char backup_tar_gz[MAX_PATH_LN];
static char working_dir[MAX_PATH_LN];
static char backup_dir[MAX_PATH_LN];
static char backup_conf[MAX_PATH_LN];
static int supress_conf_cleanup;

typedef struct path_t {
    char *str;
    struct path_t *next;
} path_t;

static path_t *path_alloc(char *str)
{
    path_t *ptr = NULL;
    int len = strlen(str) + 1;

    if (!(ptr = (path_t *)calloc(1, sizeof(path_t))))
	return NULL;
    if (!(ptr->str = (char *)calloc(len, sizeof(char))))
    {
	free(ptr);
	return NULL;
    }

    snprintf(ptr->str, len, str);
    return ptr;
}

static void path_free(path_t *ptr)
{
    if (ptr)
    {
	if (ptr->str)
	    free(ptr->str);
	free(ptr);
    }
}

static int error(char *fmt, ...)
{
#define MAX_ERR_LN 256
#define ERR_PREFIX "error: "
#define ERR_SUFFIX "\n"
    va_list ap;
    char err_str[MAX_ERR_LN];

    memset(err_str, 0, MAX_ERR_LN);
    va_start(ap, fmt);
    vsnprintf(err_str, MAX_ERR_LN, fmt, ap);
    va_end(ap);

    return fprintf(stderr, "%s%s%s", ERR_PREFIX, err_str, ERR_SUFFIX);
}

/* removes a newline if it comes as the last non zero charcater in str
 * return: 0 - if the string was not modified
 *         1 - if the string was modified */
static int remove_newline(char *str)
{
    char *c = str + strlen(str) - 1;

    if (*c == '\n')
    {
	*c = 0;
	return 1;
    }
    return 0;
}

/* adds a newline at the end of str if a the resulting string will still be null
 * terminated.
 * return: 0 - if the string was not modified
 *         1 - if the string was modified */
static int add_newline(char *str)
{
    char *c = str + strlen(str);

    if (*(c + 1) != 0)
	return 0;
    *c = '\n';
    return 1;
}

static int is_whiteline(char *line, int len)
{
    int i;

    for (i = 0; i < len && WHITESPACE(line[i]); i++);
    if (i == len)
	return 1;
    return WHITELINE(line[i]) ? 1: 0;
}

static char *del_leading_white(char *path, int len)
{
    char *c_tmp = path;

    while (path && path - c_tmp < len && WHITESPACE(*path))
	path++;
    return path;
}

static int del_obsolete_entry(char *path)
{
#define YES "yes"
#define NO "no"
/* strlen(MAX(YES, NO)) + strlen("\n") + 1(null terminator) */
#define INPUT_SZ 5

    char ans[INPUT_SZ];
    int ret = 0, modified = 0;

    printf("%s does not exist.\n", path);
    printf("Do you want to remove it from the configuration file? [Y/n] ");
    bzero(ans, INPUT_SZ);
    fgets(ans, INPUT_SZ, stdin);
    modified = remove_newline(ans);

    while (!((MAX(strlen(ans), strlen(YES)) == strlen(YES)) &&
	(!strncasecmp(ans, YES, MIN(strlen(ans), strlen(YES))))) &&
	!((MAX(strlen(ans), strlen(NO)) == strlen(NO)) &&
	 !strncasecmp(ans, NO, MIN(strlen(ans), strlen(NO)))))
    {
	/* if a newline was not encountered,
	 * it must be found and removed from stdin */
	if (!modified)
	    while(!NEWLINE(fgetc(stdin)));
	bzero(ans, INPUT_SZ);
	printf("Please answer Y[es] or n[o]: ");
	fgets(ans, INPUT_SZ, stdin);
	modified = remove_newline(ans);
    }

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
	    error("unreachable switch case");
	    exit(1);
    }
    return ret;
}

static int cp_file(FILE *to, FILE *from)
{
    char line[MAX_PATH_LN];

    bzero(line, MAX_PATH_LN);
    while (fgets(line, MAX_PATH_LN, from))
    {
	fputs(line, to);
	bzero(line, MAX_PATH_LN);
    }

    return (fseek(from, 0, SEEK_SET) || fseek(to, 0, SEEK_SET));
}

static int handler_conf_cleanup(int val, char *path, void **data)
{
    FILE *cnf = *((FILE **)data);
    char *pptr = NULL;

    add_newline(path);
    pptr = del_leading_white(path, MAX_PATH_LN);
    if (is_whiteline(path, MAX_PATH_LN))
    {
	/* if line is either whitespaces only or a comment insert it */
	fputs(path, cnf);
	goto Exit;
    }

    if (!supress_conf_cleanup && val == -1 && del_obsolete_entry(pptr))
	goto Exit;;

    fputs(path, cnf);

Exit:
    return 0;
}

static int handler_paths_create(int val, char *path, void **data)
{
    path_t *new;
    int ret = 0;

    if (val == -1)
	goto Exit;

    if (!(new = path_alloc(del_leading_white(path, MAX_PATH_LN))))
    {
	ret = -1;
	goto Exit;
    }
    new->next = *((path_t **)data);
    *data = new;

Exit:
    return ret;

}

static int file_process_generic(FILE *f, int(* handler)(int val, char *path, 
    void **data), void **data)
{
    char path[MAX_PATH_LN], *pptr = NULL;
    struct stat buf;

    bzero(path, MAX_PATH_LN);
    while (fgets(path, MAX_PATH_LN, f))
    {
	int val;

	/* line is not whitespaces only or a comment */
	pptr = del_leading_white(path, MAX_PATH_LN);
	remove_newline(pptr);
	val = stat(pptr, &buf); 
	if (val == -1 && errno != ENOENT)
	{
	    error("stat(%s, &buf), continuing...", pptr);
	    bzero(path, MAX_PATH_LN);
	    continue;
	}

	if (handler(val, path, data))
	    return -1;
	bzero(path, MAX_PATH_LN);
    }
    return 0;
}

static int conf_cleanup(FILE *cnf)
{
    char backup_conf_bck[MAX_PATH_LN];
    FILE *tmp, *bck;

    /* open tmp FILE */
    if (!(tmp = tmpfile()))
    {
	error("tmpfile()");
	return -1;
    }
    /* open bck FILE */
    snprintf(backup_conf_bck, MAX_PATH_LN, "%s.bck", backup_conf);
    if (!(bck = fopen(backup_conf_bck, "w+")))
    {
	error("fopen(%s, \"w+\")", backup_conf_bck);
	fclose(tmp);
	return -1;
    }

    /* copy conf file to backup file in case of program termination while conf
     * file is being manipulated */
    cp_file(bck, cnf);
    fclose(bck);

    /* copy conf file to tmp file and create a new conf file */
    cp_file(tmp, cnf);
    fclose(cnf);
    if (remove(backup_conf))
    {
	error("remove(%s), backup_conf");
	fclose(tmp);
	return -1;
    }
    if (!(cnf = fopen(backup_conf, "w+")))
    {
	error("fopen(%s, \"w+\")", backup_conf);
	fclose(tmp);
	return -1;
    }
    /* copy backup destinations to backup_dir and create new conf file */
    file_process_generic(tmp, handler_conf_cleanup, (void **)&cnf);
    fclose(tmp);
    fseek(cnf, 0, SEEK_SET);
    if (remove(backup_conf_bck))
    {
	error("remove(%s), backup_conf_bck");
	return -1;
    }

    return 0;
}

static int sys_exec(char *format, ...)
{
    va_list ap;
    int ret = 0, status;
    char *args[4];

    char command[MAX_PATH_LN];

    bzero(command, MAX_PATH_LN);
    va_start(ap, format);
    ret = vsnprintf(command, MAX_PATH_LN - 1, format, ap);
    va_end(ap);

    if (ret < 0)
	return -1;

    if (!fork()) /* child process */
    {
	args[0] = getenv(ENV_SHELL);
	args[1] = "-c";
	args[2] = command;
	args[3] = NULL;
	execvp(args[0], args);
    }
    wait(&status);
    if (ret < 0)
	return -1;

    return 0;
}

static int init(void)
{
    if (!(home_dir = getenv(ENV_HOME)))
    {
	error("getenv()");
	return -1;
    }
    if (!getcwd(working_dir, MAX_PATH_LN))
    {
	error("getcwd()");
	return -1;
    }
    if ((snprintf(backup_tar_gz, MAX_PATH_LN, "%s/backup.tar.gz", 
	working_dir)) < 0)
    {
	error("snprintf()");
	return -1;
    }

    if ((snprintf(backup_conf, MAX_PATH_LN, "%s/.backup.conf", home_dir) < 0))
    {
	return -1;
    }
    return 0;
}

static int create_backup_dir(void)
{
#define BACKUPDIR "backup"

    int ret, i = 0;
    char *tmp = backup_dir;

    printf("backing up...\n");
    bzero(backup_dir, MAX_PATH_LN);
    bzero(tmp, MAX_PATH_LN);
    /* add leading underscores if the backup directory allready exists */
    do
    {
	snprintf(&(tmp[i]), MAX_PATH_LN - (i + i), BACKUPDIR);
	if ((ret = mkdir(backup_dir, 0777)) == -1)
	{
	    if (errno != EEXIST)
	    {
		error("mkdir()");
		return -1;
	    }

	    tmp[i] = '_';
	}
	i++;
    }
    while (ret && (i < (MAX_PATH_LN - strlen(backup_conf) - 1)));

    return ret;
}

static int cp_to_budir(void)
{
    FILE *cnf = NULL;
    path_t *cp_paths = NULL;

    /* open cnf FILE */
    if (!(cnf = fopen(backup_conf, "r+")))
    {
	error("fopen(%s, \"r+\")", backup_conf);
	return -1;
    }
    conf_cleanup(cnf);
    /* conf file manipulation completed - no more need for backup */
    file_process_generic(cnf, handler_paths_create, (void **)&cp_paths);
    fclose(cnf);

    while (cp_paths)
    {
	path_t *tmp;

	if (sys_exec("cp -r --parents %s %s", cp_paths->str, backup_dir))
	    error("cp -r --parents %s %s", cp_paths->str, backup_dir);
	tmp = cp_paths;
	cp_paths = cp_paths->next;
	path_free(tmp);
    }
    return 0;
}

static int make_tar_gz(void)
{
    if (sys_exec("cd %s && tar czf %s .", backup_dir, backup_tar_gz))
	return -1;
    return 0;
}

static int remove_backup_dir(void)
{
    if (sys_exec("rm -rf %s", backup_dir))
	return -1;
    printf("done: %s\n", backup_tar_gz);
    return 0;
}

static int optional_backup_conf(char *file_name)
{
    struct stat s;

    if ((stat(file_name, &s) == -1) || (snprintf(backup_conf, MAX_PATH_LN, 
	"%s", file_name) < 0))
    {
	error("file %s does not exit", file_name);
	return -1;
    }
    return 0;
}

static int get_args(int argc, char *argv[])
{
#define ARG_EL "b::evhf"

    unsigned int ret = 0;
    char opt;

    while ((opt = (char)getopt(argc, argv, ARG_EL)) != -1)
    {
	switch (opt)
	{
	case 'b':
	    if (ret & ACT_BACKUP || ret & ACT_EDIT || ret & ACT_HELP || 
		ret & ACT_VERSION)
	    {
		return ACT_ERROR;
	    }
	    if (optarg && optional_backup_conf(optarg))
		return ACT_EXIT;
	    ret |= ACT_BACKUP;
	    break;
	case 'e':
	    if (ret & ACT_BACKUP || ret & ACT_EDIT || ret & ACT_HELP || 
		ret & ACT_VERSION)
	    {
		return ACT_ERROR;
	    }
	    ret |= ACT_EDIT;
	    break;
	case 'v':
	    if (ret & ACT_BACKUP || ret & ACT_EDIT || ret & ACT_HELP || 
		ret & ACT_VERSION)
	    {
		return ACT_ERROR;
	    }
	    ret |= ACT_VERSION;
	    break;
	case 'h':
	    if (ret & ACT_BACKUP || ret & ACT_EDIT || ret & ACT_HELP || 
		ret & ACT_VERSION)
	    {
		return ACT_ERROR;
	    }
	    ret |= ACT_HELP;
	    break;
	case 'f':
	    if (supress_conf_cleanup || ret & ACT_HELP || ret & ACT_VERSION)
		return ACT_ERROR;
	    supress_conf_cleanup = 1;
	    break;
	default:
	    return ACT_ERROR;
	    break;
	}
    }
    return ret;
}

static int backup(void)
{
    create_backup_dir();
    cp_to_budir();
    make_tar_gz();
    remove_backup_dir();

    return 0;
}

static int edit(void)
{
#define DEFAULT_EDITOR "vim"
#define DEFAULT_DIFFPROG "diff"

    char *editor = DEFAULT_EDITOR;
    char *diffprog = DEFAULT_DIFFPROG;
    char *env_var = NULL;
    char mode[3] = "r+", backup_conf_old[MAX_PATH_LN];
    FILE *old, *cnf;
    struct stat buf;
    int new_file = 0;

    bzero(backup_conf_old, MAX_PATH_LN);
    snprintf(backup_conf_old, MAX_PATH_LN, "%s.old", backup_conf);

    /* test if conf file exists, if not it must be created */
    if (stat(backup_conf, &buf) == -1)
    {
	snprintf(mode, 3, "w+");
	new_file = 1;
    }

    if (!(old = fopen(backup_conf_old, "w+")) || 
	!(cnf = fopen(backup_conf, mode)))
    {
	return -1;
    }

    cp_file(old, cnf);
    fclose(old);

    if ((env_var = getenv(ENV_EDITOR)))
	editor = env_var;
    if ((env_var = getenv(ENV_DIFFPROG)))
	diffprog = env_var;

    sys_exec("%s %s", editor, backup_conf);
    if (!new_file)
	sys_exec("%s %s %s", diffprog, backup_conf_old, backup_conf);
    remove(backup_conf_old);
    conf_cleanup(cnf);
    fclose(cnf);

    return 0;
}

static void version(void)
{
#define VER_LENGTH 32
    char ver[VER_LENGTH];

#ifdef VERSION
    snprintf(ver, VER_LENGTH, "%s%.4g%s", FMT_HIGHLIGHT, VERSION, FMT_RESET);
#else
    snprintf(ver, VER_LENGTH, "no data is available");
#endif

    printf("backup version: %s\n", ver);
}

static void usage(void)
{
#define COPYRIGHT 0xA9

    printf(
	"usage:	%sbackup < -e | -b [ conf_file ] > [ -f ]%s\n"
	"      	%sbackup < -v | -h >%s\n"
	"   where\n"
	"   %s-e%s  Edit the configuration file.\n"
	"	Enter the full paths to directories or files you wish to "
	"backup.\n"
	"	A line can be commented out by using the %s#%s character.\n"
	"	Set the environment variable %sEDITOR%s to use an editor of "
	"your\n"
	"	choice. If %sEDITOR%s is not set, %sbackup%s will use %svim%s "
	"for editing\n"
	"	the configuration file.\n"
	"	Once an existing configuration file is modifed, %sbackup%s\n"
	"	displays a diff between the previous and the current "
	"versions.\n"
	"	Set the environment variable %sDIFFPROG%s to use a diff "
	"program\n"
	"	of your choice. If %sDIFFPROG%s is not set, %sbackup%s will "
	"use %sGNU \n"
	"	diff%s.\n"
	"   %s-b%s  Backup the files and directories mentioned in the "
	"configuration file.\n"
	"	backup uses the default configuration file (%s) \n"
	"	unless a %sconf_file%s is stated specifically. \n"
	"	The gzipped tarball %sbackup.tar.gz%s containing the backed "
	"up files\n"
	"	will be placed in the working directory.\n"
	"   %s-f%s  Do not prompt to clean configuration file of redundent "
	"paths.\n"
	"   %s-v%s  Display %sbackup%s version.\n"
	"   %s-h%s  Print this message and exit.\n"
	"\n%s%c IAS Software, October 2004%s\n",
	FMT_HIGHLIGHT, FMT_RESET,
	FMT_HIGHLIGHT, FMT_RESET,
	FMT_HIGHLIGHT, FMT_RESET,
	FMT_HIGHLIGHT, FMT_RESET,
	FMT_UNDERLINE, FMT_RESET,
	FMT_UNDERLINE, FMT_RESET, FMT_HIGHLIGHT, FMT_RESET, FMT_HIGHLIGHT, 
	FMT_RESET,
	FMT_HIGHLIGHT, FMT_RESET,
	FMT_UNDERLINE, FMT_RESET,
	FMT_UNDERLINE, FMT_RESET, FMT_HIGHLIGHT, FMT_RESET,
	FMT_HIGHLIGHT, FMT_RESET,
	FMT_HIGHLIGHT, FMT_RESET,
	backup_conf,
	FMT_HIGHLIGHT, FMT_RESET,
	FMT_HIGHLIGHT, FMT_RESET,
	FMT_HIGHLIGHT, FMT_RESET,
	FMT_HIGHLIGHT, FMT_RESET,
	FMT_HIGHLIGHT, FMT_RESET,
	FMT_HIGHLIGHT, FMT_RESET,
	FMT_HIGHLIGHT, COPYRIGHT, FMT_RESET);
}

int main(int argc, char *argv[])
{
    int ret = 0;

    init();
    switch (get_args(argc, argv))
    {
	case ACT_BACKUP:
	    ret = backup();
	    break;
	case ACT_EDIT:
	    ret = edit();
	    break;
	case ACT_VERSION:
	    version();
	    break;
	case ACT_HELP:
	    usage();
	    break;
	case ACT_ERROR:
	    error("try 'backup -h' for more information");
	    break;
	default:
	    break;
    }
    return ret;
}
