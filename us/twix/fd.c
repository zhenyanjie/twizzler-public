#include <errno.h>
#include <twz/_slots.h>
#include <twz/obj.h>
#include <twz/view.h>

#include "syscalls.h"
static struct file fds[MAX_FD];

static struct file cur_dir;

struct file *twix_get_cwd(void)
{
	return &cur_dir;
}

static int __check_fd_valid(int fd)
{
	if(!fds[fd].valid) {
		twz_view_get(NULL, TWZSLOT_FILES_BASE + fd, NULL, &fds[fd].fl);
		if(!(fds[fd].fl & VE_VALID)) {
			return -EBADF;
		}
		fds[fd].valid = true;
		fds[fd].obj = TWZ_OBJECT_INIT(TWZSLOT_FILES_BASE + fd);
		fds[fd].taken = true;
	} else if(!fds[fd].taken) {
		return -EBADF;
	}
	return 0;
}

void __fd_sys_init(void)
{
	static bool fds_init = false;
	if(fds_init) {
		return;
	}
	fds_init = true;
	for(size_t i = 0; i < MAX_FD; i++)
		__check_fd_valid(i);
	twz_object_open_name(&cur_dir.obj, ".", FE_READ);
	cur_dir.taken = true;
	cur_dir.valid = true;
}

void twix_copy_fds(struct object *view)
{
	for(size_t i = 0; i < MAX_FD; i++) {
		struct file *f = &fds[i];
		/* TODO: CLOEXEC */
		if(f->taken) {
			objid_t fi;
			uint32_t fl;
			twz_view_get(NULL, TWZSLOT_FILES_BASE + i, &fi, &fl);
			twz_view_set(view, TWZSLOT_FILES_BASE + i, fi, fl);
		}
	}
}

struct file *twix_get_fd(int fd)
{
	return __check_fd_valid(fd) ? NULL : &fds[fd];
}

struct file *twix_alloc_fd(void)
{
	for(int i = 0; i < MAX_FD; i++) {
		int test = __check_fd_valid(i);
		if(test < 0) {
			fds[i].taken = true;
			fds[i].fd = i;
			return &fds[i];
		}
	}
	return NULL;
}