#include "syscalls.h"
#include <errno.h>
#include <fcntl.h>
#include <twz/name.h>
#include <twz/view.h>

long linux_sys_open(const char *path, int flags, int mode)
{
	objid_t id;
	int r;
	if((r = twz_name_resolve(NULL, path, NULL, 0, &id))) {
		return -ENOENT;
	}
	struct file *file = twix_alloc_fd();
	if(!file) {
		return -EMFILE;
	}
	file->fl =
	  (flags == O_RDONLY)
	    ? VE_READ
	    : 0 | (flags == O_WRONLY) ? VE_WRITE : 0 | (flags == O_RDWR) ? VE_WRITE | VE_READ : 0;

	file->taken = true;
	twz_view_set(NULL, TWZSLOT_FILES_BASE + file->fd, id, file->fl);
	file->obj = TWZ_OBJECT_INIT(TWZSLOT_FILES_BASE + file->fd);

	return file->fd;
}

long linux_sys_close(int fd)
{
	struct file *f = twix_get_fd(fd);
	if(!f) {
		return -EBADF;
	}
	f->taken = false;
	/* TODO: cleanup the file */
	return 0;
}