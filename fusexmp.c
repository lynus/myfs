#define FUSE_USE_VERSION 26
#define _XOPEN_SOURCE 700

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif
#include <assert.h>
#define MAXPATH	256
static char SSDPATH[256];
static char HDDPATH[256];
static char MP[256];
static size_t THRESH;
static unsigned long count = 0;
/*
   note 用户可以创建指向体积超过阈值的文件的符号链接，在底层文件系统表现为
   	user_sym ---> fuse_sym ---> hdd_big_file
	对两种符号链接要区别对待

*/
/*
   如果path 指向：
   SSDPATH的普通文件：一般处理
   SSDPATH的symlink，没有对应的.xattr 文件：真正的symlink文件，一般处理
   SSDPATH的symlink，有对应的.xattr 文件：正文存在hddpath中，读取.xattr文件
*/
#define getssdpath	char ssdpath[MAXPATH];	\
			strcpy(ssdpath, SSDPATH);	\
			strcat(ssdpath, path);	
#define getxattrpath	char xattrpath[MAXPATH];	\
			sprintf(xattrpath, "%s%s", SSDPATH, path);	\
			strcpy(strrchr(xattrpath, '/') + 1, ".xattr_");	\
			strcat(xattrpath, strrchr(path, '/') + 1);
#define getxattrpath_with_realssdpath(ssdpath, xattrpath)	\
			strcpy(xattrpath, ssdpath);	\
			strcpy(strrchr(xattrpath, '/') + 1, ".xattr_");	\
			strcat(xattrpath, strrchr(ssdpath, '/') + 1);

static int gethddpath(const char *path, char *hddpath)
{
	struct stat stbuf;
	char target[MAXPATH]; 
	int res;
	getssdpath;
	res = lstat(ssdpath, &stbuf);
	if (res == -1)
		return -errno;
	assert(S_ISLNK(stbuf.st_mode));
	res = readlink(ssdpath, target, MAXPATH - 1);
	if (res == -1)
		return -errno;
	target[res] = 0;
	sprintf(hddpath, "%s/%s", HDDPATH, target);
	return 0;
}

static int gethddpath_with_realssdpath(char *ssdpath, char *hddpath)
{
	struct stat stbuf;
	char target[MAXPATH]; 
	int res;
	//readlink()不给字符串追加0
	res = lstat(ssdpath, &stbuf);
	if (res == -1)
		return -errno;
	assert(S_ISLNK(stbuf.st_mode));
	res = readlink(ssdpath, target, MAXPATH - 1);
	if (res == -1)
		return -errno;
	target[res] = 0;
	sprintf(hddpath, "%s/%s", HDDPATH, target);
	return 0;
}
/* 查看是否有.xattr 文件存在
   path为fuse传来的原始路径，需要转换
*/
static int xattr_exist(const char *path)
{
	int res;
	getxattrpath;
	struct stat buf;
	res = lstat(xattrpath, &buf);
	if (res == 0) 
		return 1;
	else
		return 0;
}

static int xattr_exist_in_realssdpath(char *ssdpath)
{
	int res;
	char xattrpath[MAXPATH];
	struct stat buf;
	strcpy(xattrpath, ssdpath);
	sprintf(strrchr(xattrpath, '/') + 1, ".xattr_%s", 
			strrchr(ssdpath, '/') + 1);
	res = lstat(xattrpath, &buf);
	if (res == 0) 
		return 1;
	else
		return 0;
}

void getrealssdpath(const char *path, char *ssdpath)
{
	char linkname[MAXPATH];
	int res;
	strcpy(ssdpath, SSDPATH);
	strcat(ssdpath, path);
	if (xattr_exist_in_realssdpath(ssdpath))
		return;
	res = readlink(ssdpath, linkname, MAXPATH - 1);
	while (1) {
		if (res <= 0)
			return;
		linkname[res] = 0;
		strcpy(strrchr(ssdpath, '/') + 1, linkname);
		if (xattr_exist_in_realssdpath(ssdpath)) 
			return;
		res = readlink(ssdpath, linkname, MAXPATH - 1);
	}
}
/*
note:	原示例程序没有create方法，老版本fuse也没有这个方法。
	应该实现这个方法。因为如果没有create(), 若上层的open()里有O_CREAT的flag，
	则fuse会将这个open()转为mknod和open两个fuse方法。这样会出问题，
	因为上层的open()可能为(摘自tar命令)：
		openat(AT_FDCWD, "testlink/d1/d2/d3/lk", O_WRONLY|O_CREAT|O_EXCL, 0) 
	以上函数的permission为0，但是用本次open打开的文件是可读写的。
	而转换为mknod，则用零权限mknode，后面的open则直接失败，因为permission denied。
	所以需要实现一个create，避免以上问题
*/
static int xmp_create(const char *path, mode_t mode, struct fuse_file_info *info)
{
	int res;
	getssdpath;
	res = creat(ssdpath, mode);
	if (res == -1)
		return -errno;
	close(res);
	return 0;
}

static int xmp_getattr(const char *path, struct stat *stbuf)
{
	int res;
	getssdpath;
	res = lstat(ssdpath, stbuf);
	if (res == -1)
		return -errno;
	if (S_ISLNK(stbuf->st_mode) && xattr_exist(path)) {
		int fd;
		getxattrpath;
		fd = open(xattrpath, O_RDONLY);
		if (fd != -1) {
			res = read(fd, stbuf, sizeof(*stbuf));
			close(fd);
			if (res != sizeof(*stbuf)) 
				return -errno;
			else
				return 0;
		}
	}
	return 0;
}

static int xmp_access(const char *path, int mask)
{
/*
note:	access()会解析symlink，如果symlink无效,access()失败，errno为ENOENT
*/
	int res;
	char _path[MAXPATH];
	getssdpath;
	getrealssdpath(path, ssdpath);
	if (xattr_exist_in_realssdpath(ssdpath)) 
		gethddpath_with_realssdpath(ssdpath, _path);
	else
		strcpy(_path, ssdpath);
	res = access(_path, mask);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_readlink(const char *path, char *buf, size_t size)
{
	int res;
	char ssdpath[MAXPATH];
	strcpy(ssdpath, SSDPATH);
	strcat(ssdpath, path);
	if (xattr_exist(path)) {
		errno = EINVAL;
		return -errno;
	}
	res = readlink(ssdpath, buf, size - 1);
	if (res == -1)
		return -errno;
	buf[res] = 0;
	return 0;
}


static int xmp_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi)
{
	DIR *dp;
	struct dirent *de;

	(void) offset;
	(void) fi;
	getssdpath;
	dp = opendir(ssdpath);
	if (dp == NULL)
		return -errno;

	while ((de = readdir(dp)) != NULL) {
		struct stat st;
		if (strncmp(de->d_name, ".xattr_", 7) == 0)
			continue;
		memset(&st, 0, sizeof(st));
		st.st_ino = de->d_ino;
		st.st_mode = de->d_type << 12;
		if (filler(buf, de->d_name, &st, 0))
			break;
	}

	closedir(dp);
	return 0;
}
static int xmp_mknod(const char *path, mode_t mode, dev_t rdev)
{
	int res;
	getssdpath;

	/* On Linux this could just be 'mknod(path, mode, rdev)' but this
	   is more portable */
	if (S_ISREG(mode)) {
		res = open(ssdpath, O_CREAT | O_EXCL | O_WRONLY, mode);
		if (res >= 0)
			res = close(res);
	} else if (S_ISFIFO(mode))
		res = mkfifo(ssdpath, mode);
	else
		res = mknod(ssdpath, mode, rdev);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_mkdir(const char *path, mode_t mode)
{
	int res;
	getssdpath;
	res = mkdir(ssdpath, mode);
	if (res == -1)
		return -errno;

	return 0;
}
/*涉及三个操作，一旦一个操作出错，应该有回滚方法
  XXX 需要一些错误处理
*/
static int xmp_unlink(const char *path)
{
	int res;
	getssdpath;
	if (xattr_exist(path)) {
		char hddpath[MAXPATH];
		getxattrpath;
		res = gethddpath(path, hddpath);
		if (res != 0)
			return -errno;
		res = unlink(hddpath);
		res = unlink(xattrpath);
	}
	res = unlink(ssdpath);
	if (res == -1)
		return -errno;
	return 0;
}

static int xmp_rmdir(const char *path)
{
	int res;
	getssdpath;
	res = rmdir(ssdpath);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_symlink(const char *from, const char *to)
{
	int res;
	char ssd_to[MAXPATH];
	strcpy(ssd_to, SSDPATH);
	strcat(ssd_to, to);
	res = symlink(from, ssd_to);
	if (res == -1)
		return -errno;

	return 0;
}

/* 
   经过试验，from 和 to 都需要指向mountpoint内部才会调用rename
   因此不用再检查，可以直接添加路径前缀
*/
static int xmp_rename(const char *from, const char *to)
{
	int res;
	char ssdfrom[MAXPATH], ssdto[MAXPATH];
	strcpy(ssdfrom, SSDPATH);
	strcat(ssdfrom, from);
	strcpy(ssdto, SSDPATH);
	strcat(ssdto, to);
	res = rename(ssdfrom, ssdto);
	if (res == -1)
		return -errno;
	if (xattr_exist(from)) {
		char xattr_from[MAXPATH], xattr_to[MAXPATH];
		sprintf(xattr_from, "%s%s", SSDPATH, from);

		strcpy(strrchr(xattr_from, '/') + 1, ".xattr_");
		strcat(xattr_from, strrchr(from, '/') + 1);

		sprintf(xattr_to, "%s%s", SSDPATH, to);
		strcpy(strrchr(xattr_to, '/') + 1, ".xattr_");
		strcat(xattr_to, strrchr(to, '/') + 1);
		res = rename(xattr_from, xattr_to);
	}

	if (res == -1)
		return -errno;

	return 0;
}
/* XXX 
   有可能对fuse_symlink做link，需要xattr 同时做link
   当链接的一个文件被迁移到hdd，另一个名称也得更改。
   查找link的所有名字貌似很麻烦
   现版本不允许硬链接操作
*/
//static int xmp_link(const char *from, const char *to)
//{
//	int res;
//
//	char ssdfrom[MAXPATH], ssdto[64];
//	strcat(ssdfrom, SSDPATH);
//	strcat(ssdfrom, from);
//	strcat(ssdto, SSDPATH);
//	strcat(ssdto, to);
//	res = link(ssdfrom, ssdto);
//	if (res == -1)
//		return -errno;
//	if (xattr_exist(from)) {
//		char xattr_from[MAXPATH], xattr_to[64];
//		sprintf(xattr_from, "%s%s", SSDPATH, from);
//		strcpy(strrchr(xattr_from, '/') + 1, ".xattr_");
//		strcat(xattr_from, strrchr(from, '/') + 1);
//
//		sprintf(xattr_to, "%s%s", SSDPATH, to);
//		strcpy(strrchr(xattr_to, '/') + 1, ".xattr_");
//		strcat(xattr_to, strrchr(to, '/') + 1);
//		link(xattr_from, xattr_to);
//	}
//
//	return 0;
//}

static int xmp_chmod(const char *path, mode_t mode)
{
	int res;
	char ssdpath[MAXPATH];
	getrealssdpath(path, ssdpath);
	if (xattr_exist_in_realssdpath(ssdpath)) {
		char hddpath[MAXPATH], xattrpath[MAXPATH];
		struct stat stbuf;
		int fd;
		gethddpath_with_realssdpath(ssdpath, hddpath);
		if (chmod(hddpath, mode) == -1)
			return -errno;
		lstat(hddpath, &stbuf);
		getxattrpath_with_realssdpath(ssdpath, xattrpath);
		if ((fd = open(xattrpath, O_WRONLY)) < 0)
			return -errno;
		if ( (res = write(fd, &stbuf, sizeof(stbuf))) != sizeof(stbuf)) {
			close(fd);
			return -errno;
		}
		close(fd);
		return 0;
	} else {
		res = chmod(ssdpath, mode);
		if (res == -1)
			return -errno;

		return 0;
	}
}

static int xmp_chown(const char *path, uid_t uid, gid_t gid)
{
	int res;
	getssdpath;
	res = lchown(ssdpath, uid, gid);
	if (res == -1)
		return -errno;

	return 0;
}
/* 
   更新或者创建xattr文件
*/
static void update_create_xattr(const char *path)
{
	int fd;
	char hddpath[MAXPATH];
	struct stat stbuf;
	getssdpath;
	getxattrpath;
	lstat(ssdpath, &stbuf);
	assert(S_ISLNK(stbuf.st_mode));
	gethddpath(path, hddpath);
	assert(lstat(hddpath, &stbuf) == 0);

	fd = open(xattrpath, O_WRONLY|O_CREAT, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
	write(fd, &stbuf, sizeof(stbuf));
	close(fd);
}

static void update_create_xattr_with_realssdpath(char *ssdpath)
{
	int fd;
	char hddpath[MAXPATH], xattrpath[MAXPATH];
	struct stat stbuf;
	lstat(ssdpath, &stbuf);
	assert(S_ISLNK(stbuf.st_mode));
	gethddpath_with_realssdpath(ssdpath, hddpath);
	assert(lstat(hddpath, &stbuf) == 0);
	strcpy(xattrpath, ssdpath);
	sprintf(strrchr(xattrpath, '/') + 1, ".xattr_%s", strrchr(ssdpath, '/') + 1);

	fd = open(xattrpath, O_WRONLY|O_CREAT, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
	assert(fd > 0);
	write(fd, &stbuf, sizeof(stbuf));
	close(fd);
}
static int xmp_truncate(const char *path, off_t size)
{
//	int res;
/*
	如果size大于阈值
		如果原来小于阈值: copy源文件到hdd， truncate 删除原ssdpath文件，添加指向hddpath的symlink
		如果大于阈值：truncat 
	如果size小于阈值
		如果原来小于阈值：truncat
		如果原来大于阈值：truncat源文件， 删除symlink copy到ssd， 删除文件，删除xattr文件
*/
	char ssdpath[MAXPATH], _path[MAXPATH];
	getrealssdpath(path, ssdpath);

	if (size >= THRESH) {
		if (!xattr_exist_in_realssdpath(ssdpath)) {
			char hddpath[MAXPATH];
			struct timeval tv;
			gettimeofday(&tv, NULL);
			sprintf(hddpath, "%s/%s_%u%lu", HDDPATH, strrchr(path, '/') + 1,
					(unsigned int)tv.tv_sec,
					__sync_fetch_and_add(&count, 1));
			rename(ssdpath, hddpath);
			truncate(hddpath, size);
			unlink(ssdpath);
			symlink(strrchr(hddpath, '/') + 1, ssdpath);
		} else {
			char hddpath[MAXPATH];
			gethddpath_with_realssdpath(ssdpath, hddpath);
			truncate(hddpath, size);
		}
		update_create_xattr_with_realssdpath(ssdpath);
	} else {
		if (xattr_exist_in_realssdpath(ssdpath)) {
			char hddpath[MAXPATH], xattrpath[MAXPATH];
			getxattrpath_with_realssdpath(ssdpath, xattrpath);
			gethddpath_with_realssdpath(ssdpath, hddpath);
			truncate(hddpath, size);
			unlink(ssdpath);
			rename(hddpath, ssdpath);
			unlink(hddpath);
			unlink(xattrpath);
		} else {
			truncate(ssdpath, size);
		}
	}
	return 0;
}

static int xmp_utimens(const char *path, const struct timespec ts[2])
{
	int res;
	getssdpath;

	/* don't use utime/utimes since they follow symlinks */
	if (xattr_exist(path)) {
		//read stat buf from xattr file modify the time and write back
		struct stat stbuf;
		int fd;
		getxattrpath;
		if ((fd = open(xattrpath, O_RDWR)) < 0)
			return -errno;
		if (read(fd, &stbuf, sizeof(stbuf)) != sizeof(stbuf)) {
			close(fd);
			return -errno;
		}
		memcpy(&stbuf.st_atime, ts, sizeof(*ts));
		memcpy(&stbuf.st_mtime, &ts[1], sizeof(*ts));
		if (write(fd, &stbuf, sizeof(stbuf)) != sizeof(stbuf)) {
			close(fd);
			return -errno;
		}
		close(fd);
		return 0;
	} else {
		res = utimensat(0, ssdpath, ts, AT_SYMLINK_NOFOLLOW);
		if (res == -1)
			return -errno;
		return 0;
	}
}

static int xmp_open(const char *path, struct fuse_file_info *fi)
{
	int res;
	char _path[MAXPATH];
/* 
   XXX 需要处理trunc标志
*/
	getssdpath;
/*
note: 如果open的是一个symlink，且symlink指向的文件找不到，
	则open()失败，errno 为 2，表示文件不存在
	因此需要检测一下当前的symlink是不是指向hddpath的link
*/
	if (xattr_exist(path)) 
		gethddpath(path, _path);
	else
		strcpy(_path, ssdpath);

	res = open(_path, fi->flags);
	if (res == -1)
		return -errno;

	close(res);
	return 0;
}

static int xmp_read(const char *path, char *buf, size_t size, off_t offset,
		    struct fuse_file_info *fi)
{
	int fd;
	int res;

	(void) fi;
	char _path[MAXPATH], ssdpath[MAXPATH];
	getrealssdpath(path, ssdpath);
	if (xattr_exist_in_realssdpath(ssdpath)) {
		gethddpath_with_realssdpath(ssdpath, _path);
	} else
		strcpy(_path, ssdpath);
	fd = open(_path, O_RDONLY);
	if (fd == -1)
		return -errno;

	res = pread(fd, buf, size, offset);
	if (res == -1)
		res = -errno;

	close(fd);
	return res;
}

static int xmp_write(const char *path, const char *buf, size_t size,
		     off_t offset, struct fuse_file_info *fi)
{
	int fd;
	int res;

	(void) fi;
/*
  如果不存在xattr文件，且预计写完后体积超过阈值：
  	将文件复制到hdd，删除源文件，增加symlink，增加xattr
   再执行写入操作
*/
	char _path[MAXPATH];
	char ssdpath[MAXPATH];
	int flag = 0;
	getrealssdpath(path, ssdpath);
   	if (!xattr_exist_in_realssdpath(ssdpath)) {
		struct stat stbuf;
		res = stat(ssdpath, &stbuf);
		if (res == -1)
			return -errno;
		if (stbuf.st_size + size >= THRESH) {
			char hddpath[MAXPATH];
			struct timeval tv;
			flag = 1;
			gettimeofday(&tv, NULL);
			sprintf(hddpath, "%s/%s_%u%lu", HDDPATH, strrchr(path, '/') + 1,
					(unsigned int)tv.tv_sec,
					__sync_fetch_and_add(&count, 1));
			rename(ssdpath, hddpath);
			unlink(ssdpath);
			symlink(strrchr(hddpath, '/') + 1, ssdpath);
			strcpy(_path, hddpath);
		} else
			strcpy(_path, ssdpath);
	} else {
		flag = 1;
		gethddpath_with_realssdpath(ssdpath, _path);
	}

	fd = open(_path, O_WRONLY);
	if (fd == -1)
		return -errno;
	res = pwrite(fd, buf, size, offset);
	close(fd);
	if (flag == 1)
		update_create_xattr_with_realssdpath(ssdpath);
		
	if (res == -1)
		res = -errno;

	return res;
}
/* 暂时不支持 */
//static int xmp_statfs(const char *path, struct statvfs *stbuf)
//{
//	int res;
//
//	res = statvfs(path, stbuf);
//	if (res == -1)
//		return -errno;
//
//	return 0;
//}

static int xmp_release(const char *path, struct fuse_file_info *fi)
{
	/* Just a stub.	 This method is optional and can safely be left
	   unimplemented */

	(void) path;
	(void) fi;
	return 0;
}

static int xmp_fsync(const char *path, int isdatasync,
		     struct fuse_file_info *fi)
{
	/* Just a stub.	 This method is optional and can safely be left
	   unimplemented */

	(void) path;
	(void) isdatasync;
	(void) fi;
	return 0;
}

#ifdef HAVE_POSIX_FALLOCATE
static int xmp_fallocate(const char *path, int mode,
			off_t offset, off_t length, struct fuse_file_info *fi)
{
	int fd;
	int res;

	(void) fi;

	if (mode)
		return -EOPNOTSUPP;

	fd = open(path, O_WRONLY);
	if (fd == -1)
		return -errno;

	res = -posix_fallocate(fd, offset, length);

	close(fd);
	return res;
}
#endif

#ifdef HAVE_SETXATTR
/* xattr operations are optional and can safely be left unimplemented */
static int xmp_setxattr(const char *path, const char *name, const char *value,
			size_t size, int flags)
{
	int res = lsetxattr(path, name, value, size, flags);
	if (res == -1)
		return -errno;
	return 0;
}

static int xmp_getxattr(const char *path, const char *name, char *value,
			size_t size)
{
	int res = lgetxattr(path, name, value, size);
	if (res == -1)
		return -errno;
	return res;
}

static int xmp_listxattr(const char *path, char *list, size_t size)
{
	int res = llistxattr(path, list, size);
	if (res == -1)
		return -errno;
	return res;
}

static int xmp_removexattr(const char *path, const char *name)
{
	int res = lremovexattr(path, name);
	if (res == -1)
		return -errno;
	return 0;
}
#endif /* HAVE_SETXATTR */

static struct fuse_operations xmp_oper = {
	.getattr	= xmp_getattr,
	.access		= xmp_access,
	.readlink	= xmp_readlink,
	.readdir	= xmp_readdir,
	.create		= xmp_create,
	.mknod		= xmp_mknod,
	.mkdir		= xmp_mkdir,
	.symlink	= xmp_symlink,
	.unlink		= xmp_unlink,
	.rmdir		= xmp_rmdir,
	.rename		= xmp_rename,
//	.link		= xmp_link,
	.chmod		= xmp_chmod,
	.chown		= xmp_chown,
	.truncate	= xmp_truncate,
	.utimens	= xmp_utimens,
	.open		= xmp_open,
	.read		= xmp_read,
	.write		= xmp_write,
//	.statfs		= xmp_statfs,
	.release	= xmp_release,
	.fsync		= xmp_fsync,
/*
#ifdef HAVE_POSIX_FALLOCATE
	.fallocate	= xmp_fallocate,
#endif
#ifdef HAVE_SETXATTR
	.setxattr	= xmp_setxattr,
	.getxattr	= xmp_getxattr,
	.listxattr	= xmp_listxattr,
	.removexattr	= xmp_removexattr,
#endif
*/
};
void read_args_from_file()
{
	FILE *fp;
	if ((fp = fopen("args", "r")) == NULL) {
		perror("read arg file error!");
		exit(EXIT_FAILURE);
	}
	fscanf(fp, "%zu %s %s %s", &THRESH, SSDPATH, HDDPATH, MP);
}

	
int main(int argc, char *argv[])
{
	//umask(0);
	//srand(0xabcd);
	struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
	fuse_opt_add_arg(&args, argv[0]);
	if (argc < 5) {
		read_args_from_file();
		if (argc == 2 && strncmp(argv[1], "-d", 2) == 0)
			fuse_opt_add_arg(&args, "-d");
	} else if (argc != 5) {
		fputs("usage: ./myfs threshold ssdpath hddpath mountpoint\n", stderr);
		exit(EXIT_FAILURE);
	}else {
		THRESH = atoi(argv[1]);
		strcpy(SSDPATH, argv[2]);
		strcpy(HDDPATH, argv[3]);
		strcpy(MP, argv[4]);
	}
	fuse_opt_add_arg(&args, MP);
	return fuse_main(args.argc, args.argv, &xmp_oper, NULL);
}
