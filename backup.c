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
#define ASSERT_MKDIR 0

/* TODO: get rid of this mechanism */
#define ASSERT(RET_VAL, FUNC, FLAG) \
{ \
    switch (FLAG) \
    { \
	case (ASSERT_MKDIR): \
	    if (((RET_VAL) = (FUNC)) == -1) \
	    { \
		switch (errno) \
		{ \
		    case EPERM: \
				printf("EPERM\n"); \
			break; \
		    case EEXIST: \
				printf("EEXIST\n"); \
			 break; \
		    case EFAULT: \
				printf("EFAULT\n"); \
			 break; \
		    case EACCES: \
			 error("no writing permition"); \
		         break; \
		    case ENAMETOOLONG: \
				printf("ENAMETOOLONG\n"); \
			 break; \
		    case ENOENT: \
			error("A component used as a directory in the " \
			    "pathname is not, in fact, a directory."); \
			 break; \
		    case ENOTDIR: \
				printf("ENOTDIR\n"); \
			 break; \
		    case ENOMEM: \
				printf("ENOMEM\n"); \
			 break; \
		    case EROFS: \
				printf("EROFS\n"); \
			break; \
		    case ELOOP: \
				printf("ELOOP\n"); \
			break; \
		    case ENOSPC: \
				printf("ENOSPC\n"); \
			break; \
		    default: \
			break; \
		} \
	    } \
	    break; \
	default: \
	    error("unknown flag %s", FLAG); \
    } \
}

#define MIN(X, Y) ((X) < (Y) ? (X) : (Y))
#define MAX(X, Y) ((X) < (Y) ? (Y) : (X))
#define MAX_PATH_LN 256
#define BACKUP "backup"

static char *home_dir;
static char *backup_tar_gz = "backup.tar.gz";
static char working_dir[MAX_PATH_LN];
static char backup_dir[MAX_PATH_LN];
static char backup_conf[MAX_PATH_LN];

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

static int create_backup_dir(void)
{
    int ret, i = 0;
    char *tmp = backup_dir;

    bzero(backup_dir, MAX_PATH_LN);
    bzero(tmp, MAX_PATH_LN);
    /* add leading underscores if the backup directory allready exists */
    do
    {
	snprintf(&(tmp[i]), MAX_PATH_LN - (i + i), BACKUP);
	if ((ret = mkdir(backup_dir, S_IRUSR | S_IWUSR | S_IXUSR)) == -1)
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

static int dir_filter(const struct dirent *direp)
{
#define CURRENT_DIR "."
#define PARENT_DIR ".."

    if (!strcmp(direp->d_name, CURRENT_DIR) || 
	!strcmp(direp->d_name, PARENT_DIR))
    {
	return 0;
    }
    return 1;
}

static int remove_backup_dir(void)
{
    int fcount, prefix_ln, suffix_ln, ret;
    char path[MAX_PATH_LN], filenm[MAX_PATH_LN];
    struct dirent **namelist = NULL;

    bzero(path, MAX_PATH_LN);
    snprintf(path, MAX_PATH_LN, "%s/%s", home_dir, backup_dir);
    snprintf(filenm, MAX_PATH_LN, "%s/%s/", home_dir, backup_dir);

    if ((fcount = scandir(path, &namelist, dir_filter, alphasort)) < 0)
    {
	error("scandir()");
	return -1;
    }

    prefix_ln = strlen(filenm);
    suffix_ln = MAX_PATH_LN - prefix_ln;
    while (fcount--)
    {
	bzero(filenm + prefix_ln, suffix_ln);
	strncpy(filenm + prefix_ln, namelist[fcount]->d_name, suffix_ln - 1);
	printf("removing file %s\n", filenm);
	remove(filenm);
	free(namelist[fcount]);
    }
    free(namelist);

    ASSERT(ret, remove(path), ASSERT_MKDIR);
    return ret;
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

static int del_obsolete_entry(char *path)
{
#define YES "yes"
#define NO "no"
#define NEWLINE '\n'
#define INPUT_SZ 5 /* strlen(MAX(YES, NO)) + strlen("\n") + 1(null terminator) */

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
	/* if a NEWLINE was not encountered, it must be removed from stdin */
	if (!modified)
	    while(fgetc(stdin) != NEWLINE);
	bzero(ans, INPUT_SZ);
	printf("Please answer Y[es] or n[o]: ");
	fgets(ans, INPUT_SZ, stdin);
	modified = remove_newline(ans);
    }

    switch (*ans)
    {
	case 0: /* a NEWLINE was replaced by the null terminator */
	case 'y':
	case 'Y':
	    ret = 1;
	    break;
	case 'n':
	case 'N':
	    ret = 0;
	    break;
	default:
	    /* TODO: sanity check */
	    break;
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
    /* TODO */
    return fseek(to, 0, SEEK_SET);
}

static int sys_exec(char *format, ...)
{
    va_list ap;
    int ret = 0, pid, status;
    char *args[4];

    char command[MAX_PATH_LN];

    bzero(command, MAX_PATH_LN);
    va_start(ap, format);
    ret = vsnprintf(command, MAX_PATH_LN - 1, format, ap);
    va_end(ap);

    if (ret < 0)
	return -1;

    if (!(pid = fork())) /* child process */
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

static void cleanup(void)
{
}

static int cp_to_budir(void)
{
    FILE *tmp = NULL, *cnf = NULL;
    char path[MAX_PATH_LN];
    struct stat buf;

    /* open cnf FILE */
    if (!(cnf = fopen(backup_conf, "r+")))
    {
	error("fopen(%s, \"r+\")", backup_conf);
	return -1;
    }

    /* open tmp FILE */
    if (!(tmp = tmpfile()))
    {
	error("tmpfile()");
	return -1;
    }

    /* copy conf file to tmp file and create a new conf file */
    cp_file(tmp, cnf);
    fclose(cnf);
    if (remove(backup_conf))
    {
	error("remove()");
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
    bzero(path, MAX_PATH_LN);
    while (fgets(path, MAX_PATH_LN, tmp))
    {
	remove_newline(path);
	if (stat(path, &buf) == -1)
	{
	    if (errno != ENOENT)
	    {
		error("stat(%s, &buf), continuing...", path);
		bzero(path, MAX_PATH_LN);
		continue;
	    }
	    /* copy a non statted (non existant) path to new conf */
	    if (!del_obsolete_entry(path))
	    {
		add_newline(path);
		fputs(path, cnf);
		bzero(path, MAX_PATH_LN);
		continue;
	    }

	    bzero(path, MAX_PATH_LN);
	    continue;
	}

	if (sys_exec("cp -r --parents %s %s", path, backup_dir))
	{
	    cleanup();
	    return -1;
	}
	add_newline(path);
	fputs(path, cnf);
	bzero(path, MAX_PATH_LN);
    }

    fclose(tmp);
    fclose(cnf);
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

    if ((snprintf(backup_conf, MAX_PATH_LN, "%s/.backup.conf", home_dir) < 0))
    {
	return -1;
    }
    return 0;
}

static int make_tar_gz()
{
    if (sys_exec("tar czf %s %s", backup_tar_gz, backup_dir))
	return -1;
    return 0;
}

static int make_backup()
{
    init();
    create_backup_dir();
    cp_to_budir();
    make_tar_gz();
//    remove_backup_dir();

    return 0;
}

int main(int argc, char *argv[])
{
    make_backup();
    return 0;
}
