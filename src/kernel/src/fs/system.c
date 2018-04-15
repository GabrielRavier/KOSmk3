/* Copyright (c) 2018 Griefer@Work                                            *
 *                                                                            *
 * This software is provided 'as-is', without any express or implied          *
 * warranty. In no event will the authors be held liable for any damages      *
 * arising from the use of this software.                                     *
 *                                                                            *
 * Permission is granted to anyone to use this software for any purpose,      *
 * including commercial applications, and to alter it and redistribute it     *
 * freely, subject to the following restrictions:                             *
 *                                                                            *
 * 1. The origin of this software must not be misrepresented; you must not    *
 *    claim that you wrote the original software. If you use this software    *
 *    in a product, an acknowledgement in the product documentation would be  *
 *    appreciated but is not required.                                        *
 * 2. Altered source versions must be plainly marked as such, and must not be *
 *    misrepresented as being the original software.                          *
 * 3. This notice may not be removed or altered from any source distribution. *
 */
#ifndef GUARD_KERNEL_SRC_FS_SYSTEM_C
#define GUARD_KERNEL_SRC_FS_SYSTEM_C 1
#define _KOS_SOURCE 1
#define _GNU_SOURCE 1

#include <hybrid/compiler.h>
#include <kos/types.h>
#include <hybrid/minmax.h>
#include <kernel/sections.h>
#include <kernel/syscall.h>
#include <kernel/debug.h>
#include <kernel/malloc.h>
#include <kernel/user.h>
#include <fs/node.h>
#include <fs/linker.h>
#include <dev/wall.h>
#include <fs/handle.h>
#include <fs/path.h>
#include <fs/file.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <except.h>
#include <string.h>
#include <sched/group.h>
#include <sched/async_signal.h>
#include <sched/userstack.h>
#include <sched/posix_signals.h>
#include <bits/signum.h>
#include <bits/waitstatus.h>
#include <kernel/environ.h>
#include <sys/select.h>
#include <sys/poll.h>
#include <hybrid/align.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <linux/fs.h>
#include <kos/keymap.h>
#include <kos/keyboard-ioctl.h>
#include <linux/msdos_fs.h>
#include <sched/pertask-arith.h>

/* FS System calls. */

DECL_BEGIN

PRIVATE ATTR_NORETURN void KCALL
throw_fs_error(u16 fs_error_code) {
 struct exception_info *info;
 info = error_info();
 memset(info->e_error.e_pointers,0,sizeof(info->e_error.e_pointers));
 info->e_error.e_code                        = E_FILESYSTEM_ERROR;
 info->e_error.e_flag                        = ERR_FNORMAL;
 info->e_error.e_filesystem_error.fs_errcode = fs_error_code;
 error_throw_current();
 __builtin_unreachable();
}


DEFINE_SYSCALL0(sync) {
 superblock_syncall();
 return 0;
}

DEFINE_SYSCALL1(syncfs,int,fd) {
 REF struct superblock *block;
 block = handle_get_superblock_relaxed(fd);
 TRY {
  superblock_sync(block);
 } FINALLY {
  superblock_decref(block);
 }
 return 0;
}

#ifdef CONFIG_WIDE_64BIT_SYSCALL
DEFINE_SYSCALL5(xftruncateat,
                fd_t,dfd,USER UNCHECKED char const *,path,
                syscall_ulong_t,len_hi,
                syscall_ulong_t,len_lo,
                int,flags)
#else
DEFINE_SYSCALL4(xftruncateat,
                fd_t,dfd,USER UNCHECKED char const *,path,
                u64,length,int,flags)
#endif
{
 REF struct inode *node;
 REF struct path *p;
 if (flags & ~(FS_MODE_FKNOWNBITS))
     error_throw(E_INVALID_ARGUMENT);
 /* Lookup the user-path. */
 p = fs_pathat(dfd,path,user_strlen(path),
              &node,FS_ATMODE(flags));
 path_decref(p);
 TRY {
  /* Truncate the INode. */
#ifdef CONFIG_WIDE_64BIT_SYSCALL
  inode_truncate(node,(pos_t)len_hi << 32 | (pos_t)len_lo);
#else
  inode_truncate(node,length);
#endif
 } FINALLY {
  inode_decref(node);
 }
 return 0;
}

DEFINE_SYSCALL4(xfpathconfat,
                fd_t,dfd,USER UNCHECKED char const *,path,
                int,name,int,flags) {
 long int result;
 REF struct inode *node;
 REF struct path *p;
 if (flags & ~(FS_MODE_FKNOWNBITS))
     error_throw(E_INVALID_ARGUMENT);
 /* Lookup the user-path. */
 p = fs_pathat(dfd,path,user_strlen(path),
              &node,FS_ATMODE(flags));
 path_decref(p);
 TRY {
  /* Query information in the INode. */
  result = inode_pathconf(node,name);
 } FINALLY {
  inode_decref(node);
 }
 return result;
}

DEFINE_SYSCALL6(xfrealpathat,fd_t,dfd,
                USER UNCHECKED char const *,path,int,flags,
                USER UNCHECKED char *,buf,size_t,bufsize,
                unsigned int,type) {
 ssize_t result;
 REF struct path *p;
 if (flags & ~(FS_MODE_FKNOWNBITS))
     error_throw(E_INVALID_ARGUMENT);
 
 validate_writable(buf,bufsize);
 /* Apply the filesystem's DOS-mode to the type mask. */
 if (!(THIS_FS->fs_atmask & FS_MODE_FDOSPATH))
  type &= ~(REALPATH_FDOSPATH);
 else if (THIS_FS->fs_atflag & FS_MODE_FDOSPATH)
  type |= (REALPATH_FDOSPATH);
 /* Lookup the user-path. */
 p = fs_pathat(dfd,path,user_strlen(path),
               NULL,FS_ATMODE(flags));
 TRY {
  /* Read the filename of the path. */
  result = path_getname(p,buf,bufsize,type);
 } FINALLY {
  path_decref(p);
 }
 return result;
}

DEFINE_SYSCALL2(getcwd,USER UNCHECKED char *,buf,size_t,bufsize) {
 ssize_t result;
 REF struct path *p;
 validate_writable(buf,bufsize);
 p = fs_getcwd();
 TRY {
  /* Read the filename of the path associated with the CWD. */
  result = path_getname(p,buf,bufsize,REALPATH_FPATH);
 } FINALLY {
  path_decref(p);
 }
 return result;
}

DEFINE_SYSCALL2(fstat64,int,fd,
                USER UNCHECKED struct stat64 *,statbuf) {
 REF struct handle hnd;
 hnd = handle_get(fd);
 TRY {
  /* Query information in the handle. */
  handle_stat(hnd,statbuf);
 } FINALLY {
  handle_decref(hnd);
 }
 return 0;
}

DEFINE_SYSCALL4(fstatat64,fd_t,dfd,USER UNCHECKED char const *,path,
                USER UNCHECKED struct stat64 *,statbuf,int,flags) {
 REF struct inode *node;
 REF struct path *p;
 if (flags & ~(FS_MODE_FKNOWNBITS))
     error_throw(E_INVALID_ARGUMENT);
 /* Lookup the user-path. */
 p = fs_pathat(dfd,path,user_strlen(path),
              &node,FS_ATMODE(flags));
 path_decref(p);
 TRY {
  /* Query information in the INode. */
  inode_stat(node,statbuf);
 } FINALLY {
  inode_decref(node);
 }
 return 0;
}

DEFINE_SYSCALL4(utimensat,fd_t,dfd,USER UNCHECKED char const *,path,
                USER UNCHECKED struct timespec64 *,utimes,int,flags) {
 REF struct inode *node;
 REF struct path *p;
 if (flags & ~(FS_MODE_FKNOWNBITS|AT_CHANGE_CTIME))
     error_throw(E_INVALID_ARGUMENT);
 /* Lookup the user-path. */
 p = fs_pathat(dfd,path,user_strlen(path),
              &node,FS_ATMODE(flags));
 path_decref(p);
 TRY {
  struct timespec new_times[3]; /* 0: A; 1: M ; 2: C */
  inode_access(node,W_OK);
  /* Set the new time times. */
  if (utimes) {
   if (flags & AT_CHANGE_CTIME) {
    memcpy(new_times,utimes,3*sizeof(struct timespec));
   } else {
    memcpy(new_times,utimes,2*sizeof(struct timespec));
    new_times[2].tv_nsec = UTIME_OMIT;
   }
   if (new_times[0].tv_nsec == UTIME_NOW ||
       new_times[1].tv_nsec == UTIME_NOW ||
       new_times[2].tv_nsec == UTIME_NOW) {
    struct timespec now = wall_gettime(node->i_super->s_wall);
    if (new_times[0].tv_nsec == UTIME_NOW) new_times[0] = now;
    if (new_times[1].tv_nsec == UTIME_NOW) new_times[1] = now;
    if (new_times[2].tv_nsec == UTIME_NOW) new_times[2] = now;
   }
  } else {
   /* Use NOW for both access & modification. */
   new_times[0] = wall_gettime(node->i_super->s_wall);
   new_times[1] = new_times[0];
  }
  /* Set new file times. */
  inode_chtime(node,
               new_times[0].tv_nsec != UTIME_OMIT ? &new_times[0] : NULL,
               new_times[1].tv_nsec != UTIME_OMIT ? &new_times[1] : NULL,
               new_times[2].tv_nsec != UTIME_OMIT ? &new_times[2] : NULL);
 } FINALLY {
  inode_decref(node);
 }
 return 0;
}

#ifdef CONFIG_WIDE_64BIT_SYSCALL
DEFINE_SYSCALL3(ftruncate,
                int,fd,
                syscall_ulong_t,len_hi,
                syscall_ulong_t,len_lo)
#else
DEFINE_SYSCALL2(ftruncate,int,fd,u64,length)
#endif
{
 REF struct inode *node;
 /* Lookup the user-path. */
 node = handle_get_inode(fd);
 TRY {
  /* Truncate the INode. */
#ifdef CONFIG_WIDE_64BIT_SYSCALL
  inode_truncate(node,(pos_t)len_hi << 32 | (pos_t)len_lo);
#else
  inode_truncate(node,length);
#endif
 } FINALLY {
  inode_decref(node);
 }
 return 0;
}

DEFINE_SYSCALL2(fchmod,int,fd,mode_t,mode) {
 REF struct inode *node;
 if (mode & ~07777)
     error_throw(E_INVALID_ARGUMENT);
 node = handle_get_inode(fd);
 TRY {
  /* Change the file mode. */
  inode_chmod(node,0,mode);
 } FINALLY {
  inode_decref(node);
 }
 return 0;
}
DEFINE_SYSCALL4(fchmodat,
                fd_t,dfd,char const *,path,
                mode_t,mode,int,flags) {
 REF struct inode *node;
 REF struct path *p;
 if (mode & ~07777)
     error_throw(E_INVALID_ARGUMENT);
 if (flags & ~(FS_MODE_FKNOWNBITS))
     error_throw(E_INVALID_ARGUMENT);
 /* Lookup the user-path. */
 p = fs_pathat(dfd,path,user_strlen(path),
              &node,FS_ATMODE(flags));
 path_decref(p);
 TRY {
  /* Change the file mode. */
  inode_chmod(node,0,mode);
 } FINALLY {
  inode_decref(node);
 }
 return 0;
}

DEFINE_SYSCALL3(fchown,int,fd,uid_t,owner,gid_t,group) {
 REF struct inode *node;
 node = handle_get_inode(fd);
 TRY {
  /* Change the file owner and group. */
  inode_chown(node,owner,group);
 } FINALLY {
  inode_decref(node);
 }
 return 0;
}
DEFINE_SYSCALL5(fchownat,
                fd_t,dfd,char const *,path,
                uid_t,owner,gid_t,group,int,flags) {
 REF struct inode *node;
 REF struct path *p;
 if (flags & ~(FS_MODE_FKNOWNBITS))
     error_throw(E_INVALID_ARGUMENT);
 /* Lookup the user-path. */
 p = fs_pathat(dfd,path,user_strlen(path),
              &node,FS_ATMODE(flags));
 path_decref(p);
 TRY {
  /* Change the file owner and group. */
  inode_chown(node,owner,group);
 } FINALLY {
  inode_decref(node);
 }
 return 0;
}

PRIVATE void KCALL
translate_ioctl_error(int fd, unsigned long cmd, struct handle hnd) {
 struct exception_info *info;
 u16 expected_kind;
 if (error_code() != E_NOT_IMPLEMENTED)
     return;
#define IOCTL_TYPEOF(x) (((x) >> _IOC_TYPESHIFT) & _IOC_TYPEMASK)
 switch (IOCTL_TYPEOF(cmd)) {
 case IOCTL_TYPEOF(TCGETS):
  expected_kind = HANDLE_KIND_FTTY;
  break;
 case IOCTL_TYPEOF(BLKROSET):
  expected_kind = HANDLE_KIND_FBLOCK;
  break;
#undef IOCTL_TYPEOF
 default:
#define IOCTL_MASKOF(x) ((x) & ((_IOC_NRMASK << _IOC_NRSHIFT)|(_IOC_TYPEMASK << _IOC_TYPESHIFT)))
  switch (IOCTL_MASKOF(cmd)) {
  case IOCTL_MASKOF(KEYBOARD_ENABLE_SCANNING):
  case IOCTL_MASKOF(KEYBOARD_DISABLE_SCANNING):
  case IOCTL_MASKOF(KEYBOARD_GET_LEDS):
  case IOCTL_MASKOF(KEYBOARD_SET_LEDS):
  case IOCTL_MASKOF(KEYBOARD_GET_MODE):
  case IOCTL_MASKOF(KEYBOARD_SET_MODE):
  case IOCTL_MASKOF(KEYBOARD_GET_DELAY):
  case IOCTL_MASKOF(KEYBOARD_SET_DELAY):
  case IOCTL_MASKOF(KEYBOARD_LOAD_KEYMAP):
   expected_kind = HANDLE_KIND_FKEYBOARD;
   break;
   /* TODO: Mouse ioctls */
  case IOCTL_MASKOF(VFAT_IOCTL_READDIR_BOTH):
  case IOCTL_MASKOF(VFAT_IOCTL_READDIR_SHORT):
  case IOCTL_MASKOF(FAT_IOCTL_GET_ATTRIBUTES):
  case IOCTL_MASKOF(FAT_IOCTL_SET_ATTRIBUTES):
  case IOCTL_MASKOF(FAT_IOCTL_GET_VOLUME_ID):
   expected_kind = HANDLE_KIND_FFATFS;
   break;
  default: return;
  }
#undef IOCTL_MASKOF
  break;
 }
 info = error_info();
 info->e_error.e_code                    = E_INVALID_HANDLE;
 info->e_error.e_invalid_handle.h_handle = fd;
 info->e_error.e_invalid_handle.h_reason = ERROR_INVALID_HANDLE_FWRONGKIND;
 info->e_error.e_invalid_handle.h_istype = hnd.h_type;
 info->e_error.e_invalid_handle.h_rqtype = hnd.h_type;
 info->e_error.e_invalid_handle.h_rqkind = expected_kind;
}

DEFINE_SYSCALL3(ioctl,int,fd,unsigned long,cmd,void *,arg) {
 struct handle hnd;
 ssize_t result;
 hnd = handle_get(fd);
 TRY {
  result = handle_ioctl(hnd,cmd,arg);
 } FINALLY {
  handle_decref(hnd);
  if (FINALLY_WILL_RETHROW)
      translate_ioctl_error(fd,cmd,hnd);
 }
 return result;
}
DEFINE_SYSCALL4(xioctlf,int,fd,unsigned long,cmd,oflag_t,flags,void *,arg) {
 struct handle hnd;
 ssize_t result;
 hnd = handle_get(fd);
 TRY {
  result = handle_ioctlf(hnd,cmd,arg,flags);
 } FINALLY {
  handle_decref(hnd);
 }
 return result;
}
DEFINE_SYSCALL3(fcntl,int,fd,unsigned int,cmd,void *,arg) {
 return handle_fcntl(fd,cmd,arg);
}

DEFINE_SYSCALL5(xfreadlinkat,fd_t,dfd,
                USER UNCHECKED char const *,path,
                USER UNCHECKED char *,buf,
                size_t,len,int,flags) {
 REF struct path *p;
 REF struct symlink_node *node;
 REF struct directory_node *dir;
 size_t filename_length = user_strlen(path);
 size_t result;
 if (flags & ~FS_MODE_FKNOWNBITS)
     error_throw(E_INVALID_ARGUMENT);
 flags = FS_ATMODE(flags);
 p = fs_lastpathat(dfd,&path,&filename_length,
                  (REF struct inode **)&dir,
                   flags|FS_MODE_FDIRECTORY);
 path_decref(p);
 TRY {
  node = (struct symlink_node *)directory_getnode(dir,path,(u16)filename_length,
                                                  directory_entry_hash(path,(u16)filename_length));
 } FINALLY {
  inode_decref(&dir->d_node);
 }
 TRY {
  if (!INODE_ISLNK(&node->sl_node))
       error_throw(E_INVALID_ARGUMENT); /* XXX: Dedicated FS-error? */
  symlink_node_load(node);
  if (flags & AT_READLINK_REQSIZE) {
   /* Return the required buffer size (including a terminating NUL-character). */
   result = (size_t)node->sl_node.i_attr.a_size+1;
   COMPILER_BARRIER();
   /* Copy the link's text to user-space. */
   memcpy(buf,node->sl_text,MIN(result,len)*sizeof(char));
  } else {
   /* Why couldn't this system call just return the _required_ buffer size?
    * Then it wouldn't be a guessing game over in user-space... */
   result = (size_t)node->sl_node.i_attr.a_size;
   if (result > len) result = len;
   COMPILER_BARRIER();
   /* Copy the link's text to user-space. */
   memcpy(buf,node->sl_text,result*sizeof(char));
  }
 } FINALLY {
  inode_decref(&node->sl_node);
 }
 return result;
}

DEFINE_SYSCALL4(readlinkat,fd_t,dfd,
                USER UNCHECKED char const *,path,
                USER UNCHECKED char *,buf,
                size_t,len) {
 return SYSC_xfreadlinkat(dfd,path,buf,len,0);
}

DEFINE_SYSCALL5(xfmknodat,fd_t,dfd,
                USER UNCHECKED char const *,filename,
                mode_t,mode,dev_t,dev,int,flags) {
 REF struct directory_node *dir;
 REF struct path *p;
 REF struct inode *node;
 size_t filename_length = user_strlen(filename);
 /* Validate the mode argument. */
 if ((mode & ~(S_IFMT|0777)) ||
    ((mode & S_IFMT) != S_IFREG &&
     (mode & S_IFMT) != S_IFBLK &&
     (mode & S_IFMT) != S_IFCHR))
      error_throw(E_INVALID_ARGUMENT);
 if (flags & ~FS_MODE_FKNOWNBITS)
     error_throw(E_INVALID_ARGUMENT);
 flags = FS_ATMODE(flags);
 p = fs_lastpathat(dfd,&filename,&filename_length,
                  (REF struct inode **)&dir,
                   flags|FS_MODE_FDIRECTORY);
 path_decref(p);
 TRY {
  /* Create a new file in `dir' */
  node = directory_mknod(dir,filename,(u16)filename_length,
                         mode & ~THIS_FS->fs_umask,
                         fs_getuid(),fs_getgid(),dev,
                      !!(flags & FS_MODE_FDOSPATH));
  inode_decref(node);
 } FINALLY {
  inode_decref(&dir->d_node);
 }
 return 0;
}
DEFINE_SYSCALL4(xfmkdirat,fd_t,dfd,USER UNCHECKED char const *,filename,mode_t,mode,int,flags) {
 REF struct directory_node *dir,*node;
 REF struct path *p;
 size_t filename_length = user_strlen(filename);
 /* Validate the mode argument. */
 if (mode & ~(0777))
     error_throw(E_INVALID_ARGUMENT);
 if (flags & ~FS_MODE_FKNOWNBITS)
     error_throw(E_INVALID_ARGUMENT);
 flags = FS_ATMODE(flags);
 p = fs_lastpathat(dfd,&filename,&filename_length,
                  (REF struct inode **)&dir,
                   flags|FS_MODE_FDIRECTORY|
                   FS_MODE_FIGNORE_TRAILING_SLASHES);
 path_decref(p);
 TRY {
  /* Create a new sub-directory in `dir' */
  node = directory_mkdir(dir,filename,(u16)filename_length,
                        (mode & ~THIS_FS->fs_umask) & 0777,
                         fs_getuid(),fs_getgid(),
                      !!(flags & FS_MODE_FDOSPATH));
  inode_decref(&node->d_node);
 } FINALLY {
  inode_decref(&dir->d_node);
 }
 return 0;
}
DEFINE_SYSCALL3(unlinkat,fd_t,dfd,USER UNCHECKED char const *,filename,int,flags) {
 REF struct directory_node *dir;
 REF struct path *p; unsigned int mode;
 size_t filename_length = user_strlen(filename);
 mode = DIRECTORY_REMOVE_FREGULAR;
 /* Validate the flags argument. */
 if (flags & ~(FS_MODE_FKNOWNBITS|AT_REMOVEDIR|AT_REMOVEREG))
     error_throw(E_INVALID_ARGUMENT);
 flags = FS_ATMODE(flags);
 if (flags & AT_REMOVEDIR)
     mode  = DIRECTORY_REMOVE_FDIRECTORY,
     flags |= FS_MODE_FIGNORE_TRAILING_SLASHES;
 if (flags & AT_REMOVEREG)
     mode |= DIRECTORY_REMOVE_FREGULAR;
 if (flags & FS_MODE_FDOSPATH)
     mode |= DIRECTORY_REMOVE_FNOCASE;
 p = fs_lastpathat(dfd,&filename,&filename_length,
                  (REF struct inode **)&dir,
                   flags|FS_MODE_FDIRECTORY);
 TRY {
  /* Remove the specified filesystem object. */
  directory_remove(dir,filename,(u16)filename_length,
                   directory_entry_hash(filename,(u16)filename_length),
                   mode,p);
 } FINALLY {
  inode_decref(&dir->d_node);
  path_decref(p);
 }
 return 0;
}

DEFINE_SYSCALL4(xfsymlinkat,
                USER UNCHECKED char const *,link_text,
                fd_t,dfd,USER UNCHECKED char const *,filename,int,flags) {
 REF struct directory_node *dir;
 REF struct symlink_node *node;
 REF struct path *p;
 size_t link_text_size = user_strlen(link_text);
 size_t filename_length = user_strlen(filename);
 if (flags & ~FS_MODE_FKNOWNBITS)
     error_throw(E_INVALID_ARGUMENT);
 flags = FS_ATMODE(flags);
 p = fs_lastpathat(dfd,&filename,&filename_length,
                  (REF struct inode **)&dir,
                   flags|FS_MODE_FDIRECTORY);
 path_decref(p);
 TRY {
  /* Create the new symbolic link. */
  node = directory_symlink(dir,filename,(u16)filename_length,
                           link_text,link_text_size,
                           fs_getuid(),fs_getgid(),
                          ~THIS_FS->fs_umask,
                        !!(flags & FS_MODE_FDOSPATH));
  inode_decref(&node->sl_node);
 } FINALLY {
  inode_decref(&dir->d_node);
 }
 return 0;
}

DEFINE_SYSCALL5(linkat,
                int,olddfd,USER UNCHECKED char const *,oldname,
                int,newdfd,USER UNCHECKED char const *,newname,
                int,flags) {
 REF struct directory_node *target_dir;
 REF struct inode *link_target;
 REF struct path *p;
 size_t newname_length = user_strlen(newname);
 if (flags & ~FS_MODE_FKNOWNBITS)
     error_throw(E_INVALID_ARGUMENT);
 flags = FS_ATMODE(flags);
 p = fs_lastpathat(newdfd,&newname,&newname_length,
                  (REF struct inode **)&target_dir,
                   flags|FS_MODE_FDIRECTORY);
 path_decref(p);
 TRY {
  p = fs_pathat(olddfd,oldname,user_strlen(oldname),&link_target,
                flags|FS_MODE_FIGNORE_TRAILING_SLASHES);
  path_decref(p);
  TRY {
   /* Create the new hard link. */
   directory_link(target_dir,newname,
                 (u16)newname_length,
                  link_target,
               !!(flags & FS_MODE_FDOSPATH));
  } FINALLY {
   inode_decref(link_target);
  }
 } FINALLY {
  inode_decref(&target_dir->d_node);
 }
 return 0;
}

DEFINE_SYSCALL5(xfrenameat,
                int,olddfd,USER UNCHECKED char const *,oldname,
                int,newdfd,USER UNCHECKED char const *,newname,
                int,flags) {
 REF struct directory_node *target_dir;
 REF struct directory_node *source_dir;
 REF struct directory_entry *source_entry;
 REF struct path *p;
 size_t oldname_length = user_strlen(oldname);
 size_t newname_length = user_strlen(newname);
 if (flags & ~FS_MODE_FKNOWNBITS)
     error_throw(E_INVALID_ARGUMENT);
 flags = FS_ATMODE(flags);
 p = fs_lastpathat(newdfd,&newname,&newname_length,
                  (REF struct inode **)&target_dir,
                   flags|FS_MODE_FDIRECTORY|
                   FS_MODE_FIGNORE_TRAILING_SLASHES);
 path_decref(p);
 TRY {
  p = fs_lastpathat(olddfd,&oldname,&oldname_length,
                   (struct inode **)&source_dir,
                    flags|FS_MODE_FDIRECTORY|
                    FS_MODE_FIGNORE_TRAILING_SLASHES);
  TRY {
   if (!newname_length) {
    /* Use the filename of the original file if the target filename ends with a slash:
     * >> rename("/bar/baz.txt","/foo/");
     * Same as:
     * >> rename("/bar/baz.txt","/foo/baz.txt");
     */
    newname        = oldname;
    newname_length = oldname_length;
   }
   /* Lookup the directory entry for the source file. */
   source_entry = directory_getentry(source_dir,oldname,oldname_length,
                                     directory_entry_hash(oldname,oldname_length));
   TRY {
    /* Perform the actual rename operation. */
    directory_rename(source_dir,source_entry,
                     target_dir,newname,newname_length);
    /* Try to delete the directory for the (now removed) old filename. */
    path_delchild(p,source_dir,source_entry);
   } FINALLY {
    directory_entry_decref(source_entry);
   }
  } FINALLY {
   inode_decref(&source_dir->d_node);
   path_decref(p);
  }
 } FINALLY {
  inode_decref(&target_dir->d_node);
 }
 return 0;
}



DEFINE_SYSCALL1(dup,fd_t,fd) {
 return handle_dup(fd,0);
}
DEFINE_SYSCALL3(dup3,fd_t,oldfd,fd_t,newfd,oflag_t,flags) {
 flags = IO_HANDLE_FFROM_O(flags);
 if (flags & ~IO_HANDLE_FMASK)
     error_throw(E_INVALID_ARGUMENT);
 handle_dupinto(oldfd,newfd,(iomode_t)flags);
 return newfd;
}
DEFINE_SYSCALL1(close,fd_t,fd) {
 return handle_close(fd) ? 0 : -EBADF;
}

DEFINE_SYSCALL4(openat,
                fd_t,dfd,char const *,filename,
                oflag_t,flags,mode_t,mode) {
 REF struct inode *target_node; int result_fd;
 REF struct path *path; struct handle result;
 atflag_t at_flags;
 size_t newname_length = user_strlen(filename);
 at_flags = FS_MODE_FNORMAL;
 if (flags & O_NOFOLLOW)  at_flags |= FS_MODE_FSYMLINK_NOFOLLOW;
 if (flags & O_DIRECTORY) at_flags |= (FS_MODE_FDIRECTORY|FS_MODE_FIGNORE_TRAILING_SLASHES);
 if (flags & O_DOSPATH)   at_flags |= FS_MODE_FDOSPATH;
 at_flags = FS_ATMODE(at_flags);
 if (flags & O_CREAT) {
  if (mode & ~0777)
      error_throw(E_INVALID_ARGUMENT);
  /* Lookup the within which to create/open a file. */
  path = fs_lastpathat(dfd,
                      &filename,
                      &newname_length,
                      &target_node,
                       at_flags);
  if (!(flags & (O_SYMLINK|O_EXCL))) {
   /* TODO: If the target file is a symbolic link, traverse that link! */
  }
  TRY {
   /* Create a new file (NOTE: `O_EXCL' is handled by `file_creat()') */
   result = file_creat((struct directory_node *)target_node,path,filename,
                       (u16)newname_length,flags,fs_getuid(),fs_getgid(),
                        mode & ~THIS_FS->fs_umask);
  } FINALLY {
   inode_decref(target_node);
   path_decref(path);
  }
 } else {
  if (flags & O_SYMLINK) {
   REF struct path *filedir;
   /* `O_SYMLINK' -- Open a symbolic link.
    * Implemented by not including the last part during regular path traversal. */
   filedir = fs_lastpathat(dfd,&filename,&newname_length,
                          (REF struct inode **)&target_node,
                           at_flags);
   TRY {
    /* Lookup the last child of the path. */
    path = (at_flags&FS_MODE_FDOSPATH) ? path_casechild(filedir,filename,newname_length)
                                       : path_child(filedir,filename,newname_length);
   } FINALLY {
    if (FINALLY_WILL_RETHROW &&
        error_info()->e_error.e_code == E_FILESYSTEM_ERROR &&
        error_info()->e_error.e_filesystem_error.fs_errcode == ERROR_FS_PATH_NOT_FOUND)
        error_info()->e_error.e_filesystem_error.fs_errcode = ERROR_FS_FILE_NOT_FOUND;
    path_decref(filedir);
   }
  } else {
   path = fs_pathat(dfd,filename,newname_length,
                   (REF struct inode **)&target_node,
                    at_flags);
  }
  if (flags & O_PATH) {
   /* Open the path as a handle, not the INode itself. */
   result.h_mode = HANDLE_MODE(HANDLE_TYPE_FPATH,IO_FROM_O(flags));
   result.h_object.o_path = path;
   inode_decref(target_node);
  } else {
   TRY {
    /* Open a file stream. */
    result = file_open(target_node,path,flags);
   } FINALLY {
    inode_decref(target_node);
    path_decref(path);
   }
  }
 }
 TRY {
  /* Truncate the generated handle if the caller requested us doing so. */
  if (flags & O_TRUNC)
      handle_truncate(result,0);
  /* With a handle now opened, turn it into a descriptor. */
  result_fd = handle_put(result);
 } FINALLY {
  handle_decref(result);
 }
 return result_fd;
}

DEFINE_SYSCALL4(xreaddir,
                int,fd,USER UNCHECKED struct dirent *,buf,
                size_t,bufsize,int,mode) {
 size_t result;
 struct handle hnd;
 hnd = handle_get(fd);
 TRY {
  unsigned int mode_id;
  /* Check for read permissions. */
  if ((hnd.h_flag & IO_ACCMODE) == IO_WRONLY)
       throw_fs_error(ERROR_FS_ACCESS_ERROR);
  /* Check for known flag bits. */
  if (mode & (~READDIR_MODEMASK & ~(READDIR_FLAGMASK)))
      error_throw(E_INVALID_ARGUMENT);
  mode_id = mode & READDIR_MODEMASK;
  if (mode_id == READDIR_MULTIPLE) {
   size_t partial,alignoff;
   result = 0;
   mode &= ~(READDIR_MODEMASK);
#if READDIR_DEFAULT != 0
   mode |= READDIR_DEFAULT;
#endif
   for (;;) {
    partial = handle_readdir(hnd,buf,bufsize,mode);
    if (!partial) {
     /* Append an EOF directory entry. */
     if ((mode & READDIR_WANTEOF) && result != 0 &&
         (bufsize >= COMPILER_OFFSETOF(struct dirent,d_name)+1)) {
      buf->d_namlen  = 0;
      buf->d_name[0] = '\0';
      result += COMPILER_OFFSETOF(struct dirent,d_name)+1;
     }
     break; /* End of directory. */
    }
    if (partial > bufsize) {
     /* User-space buffer has been used up.
      * If this is the first entry that was read, return its required size. */
     if (!result) result = partial;
     break;
    }
    /* Move the buffer past this entry. */
    *(uintptr_t *)&buf += partial;
    bufsize            -= partial;
    result             += partial;
    /* Align the buffer by INodes (8 bytes). */
    alignoff = (uintptr_t)buf & (sizeof(ino64_t)-1);
    if (alignoff) {
     alignoff = sizeof(ino64_t)-alignoff;
     if (bufsize < alignoff) break;
     *(uintptr_t *)&buf += alignoff;
     bufsize            -= alignoff;
     result             += alignoff;
    }
   }
  } else {
   if (mode_id > READDIR_MODEMAX)
       error_throw(E_INVALID_ARGUMENT);
   result = handle_readdir(hnd,buf,bufsize,mode);
  }
 } FINALLY {
  handle_decref(hnd);
 }
 return result;
}

DEFINE_SYSCALL5(xreaddirf,
                int,fd,USER UNCHECKED struct dirent *,buf,
                size_t,bufsize,int,mode,oflag_t,flags) {
 size_t result;
 struct handle hnd;
 hnd = handle_get(fd);
 TRY {
  /* Check for read permissions. */
  if ((hnd.h_flag & IO_ACCMODE) == IO_WRONLY ||
      (flags & ~IO_SETFL_MASK))
       throw_fs_error(ERROR_FS_ACCESS_ERROR);
  if (mode == READDIR_MULTIPLE) {
   size_t partial,alignoff;
   result = 0;
   for (;;) {
    partial = handle_readdirf(hnd,buf,bufsize,READDIR_DEFAULT,
                            ((hnd.h_flag & ~IO_SETFL_MASK)| flags));
    if (!partial) break; /* End of directory. */
    if (partial > bufsize) {
     /* User-space buffer has been used up.
      * If this is the first entry that was read, return its required size. */
     if (!result) result = partial;
     break;
    }
    /* Move the buffer past this entry. */
    *(uintptr_t *)&buf += partial;
    bufsize            -= partial;
    result             += partial;
    /* Align the buffer by INodes (8 bytes). */
    alignoff = (uintptr_t)buf & (sizeof(ino64_t)-1);
    if (alignoff) {
     alignoff = sizeof(ino64_t)-alignoff;
     if (bufsize < alignoff) break;
     *(uintptr_t *)&buf += alignoff;
     bufsize            -= alignoff;
    }
   }
  } else {
   result = handle_readdirf(hnd,buf,bufsize,mode,
                          ((hnd.h_flag & ~IO_SETFL_MASK)| flags));
  }
 } FINALLY {
  handle_decref(hnd);
 }
 return result;
}

#ifdef CONFIG_WIDE_64BIT_SYSCALL
DEFINE_SYSCALL2_64(xfsmask,u32,mask_hi,u32,mask_lo)
#else
DEFINE_SYSCALL1_64(xfsmask,u64,mask)
#endif
{
 union fs_mask mask; u64 result;
#ifdef CONFIG_WIDE_64BIT_SYSCALL
 mask.fs_lo = mask_lo;
 mask.fs_hi = mask_hi;
#else
 mask.fs_mode = mask;
#endif
 mask.fs_atmask &= ~FS_MODE_FALWAYS0MASK;
 mask.fs_atmask |=  FS_MODE_FALWAYS1MASK;
 mask.fs_atflag &= ~FS_MODE_FALWAYS0FLAG;
 mask.fs_atflag |=  FS_MODE_FALWAYS1FLAG;
 result = ATOMIC_XCH(THIS_FS->fs_mode,mask.fs_mode);
 return result;
}

DEFINE_SYSCALL3(xfchdirat,fd_t,dfd,USER UNCHECKED char const *,reldir,int,flags) {
 REF struct path *new_path,*old_path;
 struct fs *my_fs = THIS_FS;
 if (flags & ~FS_MODE_FKNOWNBITS)
     error_throw(E_INVALID_ARGUMENT);
 new_path = fs_pathat(dfd,reldir,user_strlen(reldir),NULL,
                      FS_ATMODE(flags)|FS_MODE_FDIRECTORY|
                      FS_MODE_FIGNORE_TRAILING_SLASHES);
 atomic_rwlock_write(&my_fs->fs_lock);
 old_path = my_fs->fs_cwd;
 my_fs->fs_cwd = new_path;
 atomic_rwlock_endwrite(&my_fs->fs_lock);
 path_decref(old_path);
 return 0;
}
DEFINE_SYSCALL3(read,int,fd,USER UNCHECKED void *,buf,size_t,bufsize) {
 size_t result;
 struct handle hnd = handle_get(fd);
 TRY {
  /* Check for read permissions. */
  if ((hnd.h_flag & IO_ACCMODE) == IO_WRONLY)
       throw_fs_error(ERROR_FS_ACCESS_ERROR);
  result = handle_read(hnd,buf,bufsize);
 } FINALLY {
  handle_decref(hnd);
 }
 return result;
}
DEFINE_SYSCALL3(write,int,fd,USER UNCHECKED void const *,buf,size_t,bufsize) {
 size_t result;
 struct handle hnd = handle_get(fd);
 TRY {
  /* Check for write permissions. */
  if ((hnd.h_flag & IO_ACCMODE) == IO_RDONLY)
       throw_fs_error(ERROR_FS_ACCESS_ERROR);
  result = handle_write(hnd,buf,bufsize);
 } FINALLY {
  handle_decref(hnd);
 }
 return result;
}
DEFINE_SYSCALL4(xreadf,
                int,fd,USER UNCHECKED void *,buf,
                size_t,bufsize,oflag_t,flags) {
 size_t result;
 struct handle hnd = handle_get(fd);
 TRY {
  /* Check for read permissions. */
  if ((hnd.h_flag & IO_ACCMODE) == IO_WRONLY ||
      (flags & ~IO_SETFL_MASK))
       throw_fs_error(ERROR_FS_ACCESS_ERROR);
  result = handle_readf(hnd,buf,bufsize,
                       (hnd.h_flag & ~IO_SETFL_MASK)|
                        flags);
 } FINALLY {
  handle_decref(hnd);
 }
 return result;
}
DEFINE_SYSCALL4(xwritef,
                int,fd,USER UNCHECKED void const *,buf,
                size_t,bufsize,oflag_t,flags) {
 size_t result;
 struct handle hnd = handle_get(fd);
 TRY {
  /* Check for read permissions. */
  if ((hnd.h_flag & IO_ACCMODE) == IO_WRONLY ||
      (flags & ~IO_SETFL_MASK))
       throw_fs_error(ERROR_FS_ACCESS_ERROR);
  result = handle_writef(hnd,buf,bufsize,
                        (hnd.h_flag & ~IO_SETFL_MASK)|
                         flags);
 } FINALLY {
  handle_decref(hnd);
 }
 return result;
}

#ifdef CONFIG_WIDE_64BIT_SYSCALL
DEFINE_SYSCALL4_64(lseek,int,fd,
                   syscall_slong_t,off_hi,
                   syscall_slong_t,off_lo,
                   int,whence)
#else
DEFINE_SYSCALL3_64(lseek,int,fd,s64,off,int,whence)
#endif
{
 pos_t result;
 struct handle hnd = handle_get(fd);
 TRY {
#ifdef CONFIG_WIDE_64BIT_SYSCALL
  result = handle_seek(hnd,
                      (off_t)(((u64)off_hi << 32)|
                               (u64)off_lo),
                       whence);
#else
  result = handle_seek(hnd,off,whence);
#endif
 } FINALLY {
  handle_decref(hnd);
 }
 return result;
}

#ifdef CONFIG_WIDE_64BIT_SYSCALL
DEFINE_SYSCALL5(pread64,int,fd,
                USER UNCHECKED void *,buf,size_t,bufsize,
                syscall_ulong_t,pos_hi,
                syscall_ulong_t,pos_lo)
#else
DEFINE_SYSCALL4(pread64,int,fd,
                USER UNCHECKED void *,buf,size_t,bufsize,
                pos_t,pos)
#endif
{
 size_t result;
 struct handle hnd = handle_get(fd);
 TRY {
  /* Check for read permissions. */
  if ((hnd.h_flag & IO_ACCMODE) == IO_WRONLY)
       throw_fs_error(ERROR_FS_ACCESS_ERROR);
#ifdef CONFIG_WIDE_64BIT_SYSCALL
  result = handle_pread(hnd,buf,bufsize,
                      ((pos_t)pos_hi << 32)|
                       (pos_t)pos_lo);
#else
  result = handle_pread(hnd,buf,bufsize,pos);
#endif
 } FINALLY {
  handle_decref(hnd);
 }
 return result;
}

#ifdef CONFIG_WIDE_64BIT_SYSCALL
DEFINE_SYSCALL5(pwrite64,int,fd,
                USER UNCHECKED void const *,buf,size_t,bufsize,
                syscall_ulong_t,pos_hi,
                syscall_ulong_t,pos_lo)
#else
DEFINE_SYSCALL4(pwrite64,int,fd,
                USER UNCHECKED void const *,buf,size_t,bufsize,
                pos_t,pos)
#endif
{
 size_t result;
 struct handle hnd = handle_get(fd);
 TRY {
  /* Check for write permissions. */
  if ((hnd.h_flag & IO_ACCMODE) == IO_RDONLY)
       throw_fs_error(ERROR_FS_ACCESS_ERROR);
#ifdef CONFIG_WIDE_64BIT_SYSCALL
  result = handle_pwrite(hnd,buf,bufsize,
                       ((pos_t)pos_hi << 32)|
                        (pos_t)pos_lo);
#else
  result = handle_pwrite(hnd,buf,bufsize,pos);
#endif
 } FINALLY {
  handle_decref(hnd);
 }
 return result;
}


#ifdef CONFIG_WIDE_64BIT_SYSCALL
DEFINE_SYSCALL6(xpreadf64,int,fd,
                USER UNCHECKED void *,buf,size_t,bufsize,
                syscall_ulong_t,pos_hi,
                syscall_ulong_t,pos_lo,
                oflag_t,flags)
#else
DEFINE_SYSCALL5(xpreadf64,int,fd,
                USER UNCHECKED void *,buf,size_t,bufsize,
                pos_t,pos,oflag_t,flags)
#endif
{
 size_t result;
 struct handle hnd = handle_get(fd);
 TRY {
  /* Check for read permissions. */
  if ((hnd.h_flag & IO_ACCMODE) == IO_WRONLY ||
      (flags & ~IO_SETFL_MASK))
       throw_fs_error(ERROR_FS_ACCESS_ERROR);
#ifdef CONFIG_WIDE_64BIT_SYSCALL
  result = handle_preadf(hnd,buf,bufsize,
                       ((pos_t)pos_hi << 32)|
                        (pos_t)pos_lo,
                        (hnd.h_flag & ~IO_SETFL_MASK)|
                         flags);
#else
  result = handle_preadf(hnd,buf,bufsize,pos,
                        (hnd.h_flag & ~IO_SETFL_MASK)|
                         flags);
#endif
 } FINALLY {
  handle_decref(hnd);
 }
 return result;
}

#ifdef CONFIG_WIDE_64BIT_SYSCALL
DEFINE_SYSCALL6(xpwritef64,int,fd,
                USER UNCHECKED void const *,buf,size_t,bufsize,
                syscall_ulong_t,pos_hi,
                syscall_ulong_t,pos_lo,oflag_t,flags)
#else
DEFINE_SYSCALL5(xpwritef64,int,fd,
                USER UNCHECKED void const *,buf,size_t,bufsize,
                pos_t,pos,oflag_t,flags)
#endif
{
 size_t result;
 struct handle hnd = handle_get(fd);
 TRY {
  /* Check for write permissions. */
  if ((hnd.h_flag & IO_ACCMODE) == IO_RDONLY ||
      (flags & ~IO_SETFL_MASK))
       throw_fs_error(ERROR_FS_ACCESS_ERROR);
#ifdef CONFIG_WIDE_64BIT_SYSCALL
  result = handle_pwritef(hnd,buf,bufsize,
                        ((pos_t)pos_hi << 32)|
                         (pos_t)pos_lo,
                         (hnd.h_flag & ~IO_SETFL_MASK)|
                          flags);
#else
  result = handle_pwritef(hnd,buf,bufsize,pos,
                         (hnd.h_flag & ~IO_SETFL_MASK)|
                          flags);
#endif
 } FINALLY {
  handle_decref(hnd);
 }
 return result;
}


PRIVATE syscall_ulong_t KCALL
do_fsync(fd_t fd, bool data_only) {
 struct handle hnd = handle_get(fd);
 TRY {
  handle_sync(hnd,data_only);
 } FINALLY {
  handle_decref(hnd);
 }
 return 0;
}

DEFINE_SYSCALL1(fsync,int,fd) {
 return do_fsync(fd,false);
}
DEFINE_SYSCALL1(fdatasync,int,fd) {
 return do_fsync(fd,true);
}
DEFINE_SYSCALL0(xgetdrives) {
 /* Return a bit-set of all mounted DOS drives. */
 u32 result = 0; unsigned int i;
 struct vfs *v = THIS_VFS;
 atomic_rwlock_read(&v->v_drives.d_lock);
 for (i = 0; i < VFS_DRIVECOUNT; ++i) {
  if (v->v_drives.d_drives[i])
      result |= (u32)1 << i;
 }
 atomic_rwlock_endread(&v->v_drives.d_lock);
 return result;
}


PRIVATE void KCALL set_exit_reason(int exitcode) {
 struct exception_info *reason;
 reason = error_info();
 memset(reason->e_error.e_pointers,0,sizeof(reason->e_error.e_pointers));
 reason->e_error.e_flag          = ERR_FNORMAL;
 reason->e_error.e_exit.e_status = __W_EXITCODE(exitcode,0);
}

DEFINE_SYSCALL1(exit,int,exitcode) {
 set_exit_reason(exitcode);
 error_info()->e_error.e_code = E_EXIT_THREAD;
 error_throw_current();
 __builtin_unreachable();
}

DEFINE_SYSCALL1(exit_group,int,exitcode) {
 set_exit_reason(exitcode);
 error_info()->e_error.e_code = E_EXIT_PROCESS;
 error_throw_current();
 __builtin_unreachable();
}



/* Link compatibility system calls. */
DEFINE_SYSCALL4(mknodat,fd_t,dfd,USER UNCHECKED char const *,filename,mode_t,mode,dev_t,dev) {
 return SYSC_xfmknodat(dfd,filename,mode,dev,FS_MODE_FNORMAL);
}
DEFINE_SYSCALL3(mkdirat,fd_t,dfd,USER UNCHECKED char const *,filename,mode_t,mode) {
 return SYSC_xfmkdirat(dfd,filename,mode,FS_MODE_FNORMAL);
}
DEFINE_SYSCALL1(chdir,USER UNCHECKED char const *,reldir) {
 return SYSC_xfchdirat(HANDLE_SYMBOLIC_CWD,reldir,FS_MODE_FNORMAL);
}
DEFINE_SYSCALL1(chroot,USER UNCHECKED char const *,reldir) {
 /* In kos, implemented using open() + dup2() */
 REF struct path *new_path,*old_path;
 struct fs *my_fs = THIS_FS;
 new_path = fs_path(NULL,reldir,user_strlen(reldir),NULL,
                    FS_DEFAULT_ATMODE|FS_MODE_FDIRECTORY|
                    FS_MODE_FIGNORE_TRAILING_SLASHES);
 atomic_rwlock_write(&my_fs->fs_lock);
 old_path = my_fs->fs_root;
 my_fs->fs_root = new_path;
 atomic_rwlock_endwrite(&my_fs->fs_lock);
 path_decref(old_path);
 return 0;
}
DEFINE_SYSCALL1(fchdir,int,fd) {
 /* In kos, implemented using `dup2()' */
 REF struct path *new_path,*old_path;
 struct fs *my_fs = THIS_FS;
 new_path = handle_get_path(fd);
 atomic_rwlock_write(&my_fs->fs_lock);
 old_path = my_fs->fs_cwd;
 my_fs->fs_cwd = new_path;
 atomic_rwlock_endwrite(&my_fs->fs_lock);
 path_decref(old_path);
 return 0;
}
DEFINE_SYSCALL4(renameat,
                int,olddfd,USER UNCHECKED char const *,oldname,
                int,newdfd,USER UNCHECKED char const *,newname) {
 return SYSC_xfrenameat(olddfd,oldname,newdfd,newname,FS_MODE_FNORMAL);
}
DEFINE_SYSCALL3(symlinkat,
                USER UNCHECKED char const *,link_text,
                fd_t,dfd,USER UNCHECKED char const *,filename) {
 return SYSC_xfsymlinkat(link_text,dfd,filename,FS_MODE_FNORMAL);
}
#ifdef CONFIG_WIDE_64BIT_SYSCALL
DEFINE_SYSCALL3(truncate,
                USER UNCHECKED char const *,path,
                syscall_ulong_t,len_hi,
                syscall_ulong_t,len_lo) {
 return SYSC_xftruncateat(AT_FDCWD,path,len_hi,len_lo,FS_MODE_FNORMAL);
}
#else
DEFINE_SYSCALL2(truncate,USER UNCHECKED char const *,path,u64,length) {
 return SYSC_xftruncateat(AT_FDCWD,path,length,FS_MODE_FNORMAL);
}
#endif


struct exec_args {
    REF struct module                   *ea_module;
    USER UNCHECKED char *USER UNCHECKED *ea_argv;
    USER UNCHECKED char *USER UNCHECKED *ea_envp;
};


PRIVATE void KCALL
exec_user(struct exec_args *__restrict args,
          struct cpu_hostcontext_user *__restrict context,
          unsigned int UNUSED(mode)) {
 REF struct application *app;
 REF struct module *mod;
 USER UNCHECKED char *USER UNCHECKED *argv;
 USER UNCHECKED char *USER UNCHECKED *envp;
 /* Load arguments and free their buffer. */
 mod = args->ea_module;
 argv = args->ea_argv;
 envp = args->ea_envp;
 kfree(args);
 TRY {
  /* Construct a new application. */
  app = application_alloc();
  app->a_module = mod; /* Inherit reference. */
 } EXCEPT(EXCEPT_EXECUTE_HANDLER) {
  module_decref(mod);
  error_rethrow();
 }
 TRY {
  struct userstack *stack;
  REF struct vm_node *environ_node;
  struct process_environ *environ_address;
  /* Terminate all other threads running in the current process.
   * NOTE: This function is executed in the context of the process leader. */
  task_exit_secondary_threads(__W_EXITCODE(0,0));

  /* Construct an application environment mapping. */
  environ_node = environ_alloc(argv,envp);
  TRY {
   /* Reset all signal actions of the calling thread to their default disposition. */
   signal_resetexec();
   /* With that out of the way, unmap _everything_ from user-space. */
   vm_unmap_userspace();
   pagedir_syncall();

   /* Update the thread configuration to indicate that stack and segments are gone. */
   PERTASK_AND(this_task.t_flags,~(TASK_FOWNUSERSEG));
   {
    REF struct userstack *stack;
    stack = PERTASK_XCH(_this_user_stack,NULL);
    /* XXX: Re-use the user-space stack object. */
    if (stack) userstack_decref(stack);
   }

   /* Setup the application. */
   application_loadroot(app,
                        DL_OPEN_FGLOBAL,
                        "/lib:/usr/lib" /* XXX: This should be taken from environ. */
                        );

   /* Fix the protection of the environment
    * node to allow for user-space access. */
   environ_node->vn_prot = PROT_READ|PROT_WRITE;

   /* Map the new environment region. */
   vm_acquire(THIS_VM);
   TRY {
    vm_vpage_t environ_page;
    environ_page = vm_getfree(VM_USERENV_HINT,
                              VM_NODE_SIZE(environ_node),
                              1,
                              0,
                              VM_USERENV_MODE);
    /* Save the address of the new environment table. */
    environ_address = (struct process_environ *)VM_PAGE2ADDR(environ_page);
    PERVM(vm_environ) = environ_address;
    /* Update the ranges of the environment node. */
    environ_node->vn_node.a_vmax -= environ_node->vn_node.a_vmin;
    environ_node->vn_node.a_vmin  = environ_page;
    environ_node->vn_node.a_vmax += environ_page;
    /* Insert + map the new node in user-space. */
    vm_insert_and_activate_node(THIS_VM,environ_node);
   } FINALLY {
    vm_release(THIS_VM);
   }
  } EXCEPT (EXCEPT_EXECUTE_HANDLER) {
   vm_region_decref_range(environ_node->vn_region,
                          environ_node->vn_start,
                          VM_NODE_SIZE(environ_node));
   vm_region_decref(environ_node->vn_region);
   vm_node_free(environ_node);
   error_rethrow();
  }

  /* Relocate the environment table to its proper address. */
  environ_relocate(environ_address);

  /* The new application has now been loaded.
   * Allocate the user-space task segment and a new stack. */
  task_alloc_userseg();
  set_user_tls_register(PERTASK_GET(this_task.t_userseg));
  stack = task_alloc_userstack();

  /* Finally, update the user-space CPU context
   * to enter at the new application's entry point. */
  CPU_CONTEXT_IP(*context) = app->a_loadaddr + app->a_module->m_entry;
#ifdef CONFIG_STACK_GROWS_UPWARDS
  CPU_CONTEXT_SP(*context) = VM_PAGE2ADDR(stack->us_pagemin);
#else
  CPU_CONTEXT_SP(*context) = VM_PAGE2ADDR(stack->us_pageend);
#endif
  /* Queue execution of library initializers
   * for all currently loaded modules. */
  vm_apps_initall(context);

 } FINALLY {
  application_decref(app);
 }
 error_info()->e_error.e_code = E_USER_RESUME;
}


/* Linker functions. */
DEFINE_SYSCALL5(execveat,fd_t,dfd,
                USER UNCHECKED char const *,filename,
                USER UNCHECKED char **,argv,
                USER UNCHECKED char **,envp,int,flags) {
 REF struct module *COMPILER_IGNORE_UNINITIALIZED(exec_module);
 REF struct path *exec_path; REF struct inode *exec_node;
 struct exec_args *args;
 if (flags & ~(FS_MODE_FKNOWNBITS))
     error_throw(E_INVALID_ARGUMENT);
 /* Load the file that should be executed. */
 exec_path = fs_pathat(dfd,filename,user_strlen(filename),
                      &exec_node,FS_ATMODE(flags));
 TRY {
  if (!INODE_ISREG(exec_node))
       error_throw(E_NOT_EXECUTABLE);
  /* Check for execute and read permissions. */
  inode_access(exec_node,R_OK|X_OK);
  /* Open a new module under the given path. */
  exec_module = module_open((struct regular_node *)exec_node,exec_path);
 } FINALLY {
  inode_decref(exec_node);
  path_decref(exec_path);
 }
 /* XXX: Copy `argv' and `envp' into kernel-space,
  *      or somehow preserve their memory. */
 TRY {
  args = (struct exec_args *)kmalloc(sizeof(struct exec_args),GFP_SHARED);
  args->ea_module = exec_module;
  args->ea_argv   = argv;
  args->ea_envp   = envp;
 } EXCEPT (EXCEPT_EXECUTE_HANDLER) {
  module_decref(exec_module);
  error_rethrow();
 }
 TRY {
  /* Check if the module actually has an entry point. */
  if (!(exec_module->m_flags & MODULE_FENTRY))
        error_throw(E_NOT_EXECUTABLE);

  /* Send a request to the. */
  if (!task_queue_rpc_user(get_this_process(),(task_user_rpc_t)&exec_user,
                           args,TASK_RPC_SYNC|TASK_RPC_USER))
       error_throw(E_INTERRUPT); /* If the leader has been terminated, we'll be too sooner or later. */
  else {
   /* We shouldn't actually get here because the RPC should have terminated
    * all other threads upon success (which would have included us.) */
   task_serve();
  }
 } EXCEPT (EXCEPT_EXECUTE_HANDLER) {
  /* If we got here because of the INTERRUPT-exception thrown when a thread
   * tries to send an RPC to itself, then the RPC will have still been scheduled
   * successfully, meaning we must not decref() the module passed to the RPC function. */
  if (error_code() == E_INTERRUPT &&
      get_this_process() == THIS_TASK)
      error_rethrow();
  debug_printf("EXEC FAILED\n");
  /* NOTE: Upon success, `task_queue_rpc_user()' and the `exec_user()' function
   *       call will have inherited a reference to the exec-module. */
  module_decref(exec_module);
  kfree(args);
  error_rethrow();
 }
 return 0;
}

DEFINE_SYSCALL3(execve,
                USER UNCHECKED char const *,filename,
                USER UNCHECKED char **,argv,
                USER UNCHECKED char **,envp) {
 return SYSC_execveat(AT_FDCWD,filename,argv,envp,0);
}

DEFINE_SYSCALL1(umask,mode_t,mask) {
 /* Simply exchange the UMASK of the calling thread. */
 return ATOMIC_XCH(THIS_FS->fs_umask,mask & S_IRWXUGO);
}

DEFINE_SYSCALL5(ppoll,
                USER UNCHECKED struct pollfd *,ufds,size_t,nfds,
                USER UNCHECKED struct timespec const *,tsp,
                USER UNCHECKED sigset_t const *,sigmask,
                size_t,sigsetsize) {
 size_t result,i,insize; sigset_t old_blocking;
 /* */if (!sigmask);
 else if (sigsetsize > sizeof(sigset_t)) {
  error_throw(E_INVALID_ARGUMENT);
 }
 if (__builtin_mul_overflow(nfds,sizeof(struct pollfd),&insize))
     insize = (size_t)-1;
 validate_readable(ufds,insize);
 validate_readable_opt(tsp,sizeof(*tsp));
 if (sigmask)
     signal_chmask(sigmask,&old_blocking,sigsetsize,SIGNAL_CHMASK_FBLOCK);
 TRY {
scan_again:
  result = 0;
  /* Clear the channel mask. Individual channels
   * may be re-opened by poll-callbacks as needed. */
  task_channelmask(0);
  for (i = 0; i < nfds; ++i) {
   struct handle hnd;
   TRY {
    hnd = handle_get(ufds[i].fd);
    TRY {
     unsigned int mask;
     size_t num_connections = task_numconnected();
     mask = handle_poll(hnd,ufds[i].events);
     if (mask) {
      ++result;
      ufds[i].revents = (u16)mask;
     } else if (num_connections == task_numconnected()) {
      /* This handle didn't add any new connections,
       * and neither are any of its states signaled.
       * As a conclusion: this handle doesn't support poll() */
      ufds[i].revents = POLLNVAL;
     } else {

      ufds[i].revents = 0;
     }
    } FINALLY {
     handle_decref(hnd);
    }
   } CATCH (E_INVALID_HANDLE) {
    ufds[i].revents = POLLERR;
   }
  }
  if (result) {
   /* At least one of the handles has been signaled. */
   task_udisconnect();
  } else if (!tsp) {
   task_uwait();
   goto scan_again;
  } else {
   /* Wait for signals to arrive and scan again. */
   if (task_uwaitfor_tmabs(tsp))
       goto scan_again;
   /* NOTE: If the timeout expires, ZERO(0) is returned. */
  }
 } FINALLY {
  if (sigmask)
      signal_chmask(&old_blocking,NULL,sigsetsize,SIGNAL_CHMASK_FBLOCK);
 }
 return result;
}

struct pselect6_sig {
    USER UNCHECKED sigset_t *set;
    size_t                   setsz;
};

DEFINE_SYSCALL6(pselect6,size_t,n,
                USER UNCHECKED fd_set *,inp,
                USER UNCHECKED fd_set *,outp,
                USER UNCHECKED fd_set *,exp,
                USER UNCHECKED struct timespec const *,tsp,
                USER UNCHECKED struct pselect6_sig *,sig) {
 unsigned int result;
 sigset_t old_blocking;
 validate_readable_opt(sig,sizeof(*sig));
 validate_readable_opt(inp,CEILDIV(n,8));
 validate_readable_opt(outp,CEILDIV(n,8));
 validate_readable_opt(exp,CEILDIV(n,8));
 if (sig) {
  if (!sig->set) sig = NULL;
  else {
   signal_chmask(&old_blocking,sig->set,
                  sig->setsz,SIGNAL_CHMASK_FBLOCK);
  }
 }
 TRY {
  unsigned int fd_base;
scan_again:
  result = 0;
  /* Clear the channel mask. Individual channels
   * may be re-opened by poll-callbacks as needed. */
  task_channelmask(0);
  for (fd_base = 0; fd_base < n; fd_base += 8) {
   u8 part_i,part_o,part_e,mask; size_t fd_no;
   size_t part_bits = MIN(n-fd_base,8);
   size_t old_result = result;
   /* Figure out which descriptors we're supposed to wait for. */
   part_i = part_o = part_e = 0;
   if (inp)  part_i = *(u8 *)((byte_t *)inp +(fd_base/8));
   if (outp) part_o = *(u8 *)((byte_t *)outp +(fd_base/8));
   if (exp)  part_e = *(u8 *)((byte_t *)exp +(fd_base/8));
   /* Quick check: If we're not supposed to wait for anything, skip ahead. */
   if (!part_i && !part_o && !part_e) continue;
   for (fd_no = fd_base,mask = 1; part_bits; mask <<= 1,++fd_no,--part_bits) {
    unsigned int mode = 0; REF struct handle hnd;
    if (part_i & mask) mode |= POLLIN|POLLPRI;
    if (part_o & mask) mode |= POLLOUT;
    if (part_e & mask) mode |= POLLERR;
    if (!mode) continue;
    hnd = handle_get(fd_no);
    TRY {
     mode = handle_poll(hnd,mode);
    } FINALLY {
     handle_decref(hnd);
    }
    part_i &= ~mask;
    part_o &= ~mask;
    part_e &= ~mask;
    if (mode & (POLLIN|POLLPRI)) part_i |= mask,++result;
    if (mode & (POLLOUT))        part_o |= mask,++result;
    if (mode & (POLLERR))        part_e |= mask,++result;
   }
   /* Write back changes. */
   if (result) {
    /* Clear out all files that were never signaled. */
    if (!old_result && fd_base != 0) {
     if (inp)  memset(inp,0,fd_base/8);
     if (outp) memset(outp,0,fd_base/8);
     if (exp)  memset(exp,0,fd_base/8);
    }
    /* Update what is now available. */
    if (inp)  *(u8 *)((byte_t *)inp +(fd_base/8))  = part_i;
    if (outp) *(u8 *)((byte_t *)outp +(fd_base/8)) = part_o;
    if (exp)  *(u8 *)((byte_t *)exp +(fd_base/8))  = part_e;
   }
  }
  if (result) {
   /* Disconnect from all connected signals. */
   task_udisconnect();
  } else if (!tsp) {
   task_uwait();
   goto scan_again;
  } else {
   /* Wait for files to become ready. */
   if (task_uwaitfor_tmabs(tsp))
       goto scan_again;
   /* NOTE: If the timeout expires, ZERO(0) is returned. */
  }
 } FINALLY {
  if (sig)
      signal_chmask(&old_blocking,NULL,sig->setsz,SIGNAL_CHMASK_FBLOCK);
 }
 return result;
}


#ifdef CONFIG_WIDE_64BIT_SYSCALL
DEFINE_SYSCALL6(fallocate,
                int,fd,int,mode,
                syscall_ulong_t,off_hi,
                syscall_ulong_t,off_lo,
                syscall_ulong_t,len_hi,
                syscall_ulong_t,len_lo)
#else
DEFINE_SYSCALL4(fallocate,
                int,fd,int,mode,
                syscall_ulong_t,off,
                syscall_ulong_t,len)
#endif
{
 struct handle hnd;
 hnd = handle_get(fd);
 TRY {
#ifdef CONFIG_WIDE_64BIT_SYSCALL
  handle_allocate(hnd,mode,
                ((u64)off_hi << 32 | (u64)off_lo),
                ((u64)len_hi << 32 | (u64)len_lo));
#else
  handle_allocate(hnd,mode,off,len);
#endif
 } FINALLY {
  handle_decref(hnd);
 }
 return 0;
}


PRIVATE void KCALL
dlinit_rpc(void *arg,
           struct cpu_hostcontext_user *__restrict context,
           unsigned int UNUSED(mode)) {
#if defined(__x86_64__)
 context->c_gpregs.gp_rax = (uintptr_t)arg;
#elif defined(__i386__)
 context->c_gpregs.gp_eax = (uintptr_t)arg;
#else
#error "Unsupported architecture"
#endif
 vm_apps_initall(context);
 error_info()->e_error.e_code = E_USER_RESUME;
}


DEFINE_SYSCALL5(xfdlopenat,
                fd_t,dfd,USER UNCHECKED char const *,filename,
                int,at_flags,int,open_flags,char const *,runpath) {
 void *result;
 REF struct path *module_path;
 REF struct inode *module_inode;
 REF struct module *mod;
 REF struct application *root_app;
 REF struct application *result_app;
 /* Validate arguments. */
 validate_readable_opt(runpath,1);
 if ((at_flags & ~FS_MODE_FKNOWNBITS) ||
     (open_flags & ~DL_OPEN_FMASK))
      error_throw(E_INVALID_ARGUMENT);
 if (!runpath)
      runpath = "/lib:/usr/lib"; /* XXX: Take from environ? */
 at_flags = FS_ATMODE(at_flags);

 /* XXX: Search the library path? */
 module_path = fs_pathat(dfd,filename,user_strlen(filename),
                        &module_inode,at_flags);
 TRY {
  if (!INODE_ISREG(module_inode))
       error_throw(E_NOT_EXECUTABLE);
  /* Open a module under the given path. */
  mod = module_open((struct regular_node *)module_inode,module_path);
 } FINALLY {
  inode_decref(module_inode);
  path_decref(module_path);
 }
 TRY {
again:
  vm_acquire_read(THIS_VM);
  TRY {
   root_app = vm_apps_primary(THIS_VM);
   if unlikely(!root_app) {
    /* No root application? This seems fishy, but ok... */
    result_app = application_alloc();
    TRY {
     module_incref(mod);
     result_app->a_module = mod;
     result_app->a_flags |= APPLICATION_FDYNAMIC;
     application_loadroot(result_app,
                          DL_OPEN_FGLOBAL,
                          runpath);
    } EXCEPT (EXCEPT_EXECUTE_HANDLER) {
     application_decref(result_app);
     error_rethrow();
    }
   } else {
    struct module_patcher patcher;
    /* Now load the module in the current VM (as a dependency of the root application). */
    patcher.mp_root     = &patcher;
    patcher.mp_prev     = NULL;
    patcher.mp_app      = root_app;
    patcher.mp_requirec = 0;
    patcher.mp_requirea = 0;
    patcher.mp_requirev = NULL;
    patcher.mp_runpath  = runpath;
    patcher.mp_altpath  = NULL;
    patcher.mp_flags    = open_flags;
    patcher.mp_apptype  = APPLICATION_TYPE_FUSERAPP;
    patcher.mp_appflags = APPLICATION_FDYNAMIC;
    TRY {
     /* Open the module user the new patcher. */
     result_app = patcher_require(&patcher,mod);
     application_incref(result_app);
    } FINALLY {
     patcher_fini(&patcher);
    }
   }
  } FINALLY {
   if (vm_release_read(THIS_VM))
       goto again;
  }
 } FINALLY {
  module_decref(mod);
 }
 /* Got the resulting application! */
 ATOMIC_FETCHINC(result_app->a_loadcnt);
 result = (void *)APPLICATION_MAPBEGIN(result_app);
 application_decref(result_app);
 /* Queue library initializers (s.a. `vm_apps_initall()'). */
 task_queue_rpc_user(THIS_TASK,
                    &dlinit_rpc,
                     result,
                     TASK_RPC_USER|
                     TASK_RPC_SINGLE);
 /* XXX: Don't get here? */
 return (syscall_ulong_t)result;
}


DEFINE_SYSCALL1(xdlclose,void *,handle) {
 struct application *app;
 app = vm_getapp(handle);
 TRY {
  if (ATOMIC_DECIFNOTZERO(app->a_mapcnt) &&
     (app->a_flags & APPLICATION_FDYNAMIC)) {
   /* TODO: recursively find all unreachable (not part of dependencies)
    *       applications and queue their finalizers in user-space.
    *       Once those are executed, return (using a #PF-syscall) to
    *       kernel space and unmap all of their memory.
    *       Then (finally) return back to the caller.
    */
  }
 } FINALLY {
  application_decref(app);
 }
 return 0;
}

DEFINE_SYSCALL2(xdlsym,void *,handle,
                USER UNCHECKED char const *,symbol) {
 struct module_symbol sym;
 validate_readable(symbol,1);
 if (!handle) {
  sym = vm_apps_dlsym(symbol);
 } else {
  REF struct application *app;
  app = vm_getapp(handle);
  TRY {
   /* Lookup a symbol within the specified application. */
   sym = application_dlsym(app,symbol);
  } FINALLY {
   application_decref(app);
  }
 }
 /* Check if a symbol was found. */
 if (sym.ms_type == MODULE_SYMBOL_INVALID)
     error_throw(E_NO_SUCH_OBJECT);
 return (syscall_ulong_t)sym.ms_base;
}



PRIVATE void KCALL
dlfini_rpc(void *UNUSED(arg),
           struct cpu_hostcontext_user *__restrict context,
           unsigned int UNUSED(mode)) {
 vm_apps_finiall(context);
 error_info()->e_error.e_code = E_USER_RESUME;
}

DEFINE_SYSCALL0(xdlfini) {
 task_queue_rpc_user(THIS_TASK,
                    &dlfini_rpc,
                     NULL,
                     TASK_RPC_USER|
                     TASK_RPC_SINGLE);
 return 0; /* XXX: Don't get here? */
}

DEFINE_SYSCALL4(xdlmodule_info,void *,handle,
                unsigned int,info_class,
                void *,buf,size_t,bufsize) {
 REF struct application *app;
 size_t result;
 app = vm_getapp(handle);
 TRY {
  switch (info_class) {

  {
   struct module_basic_info *info;
  case MODULE_INFO_CLASS_BASIC:
   result = sizeof(struct module_basic_info);
   if (bufsize < sizeof(struct module_basic_info))
       break;
   info = (struct module_basic_info *)buf;
   info->mi_loadaddr = app->a_loadaddr;
   info->mi_segstart = APPLICATION_MAPBEGIN(app);
   info->mi_segend   = APPLICATION_MAPEND(app);
  } break;

  {
   struct module_state_info *info;
  case MODULE_INFO_CLASS_STATE:
   result = sizeof(struct module_state_info);
   if (bufsize < sizeof(struct module_state_info))
       break;
   info = (struct module_state_info *)buf;
   info->si_mapcnt   = app->a_mapcnt;
   info->si_loadcnt  = app->a_loadcnt;
   info->si_appflags = app->a_flags;
  } break;

  default:
   error_throw(E_INVALID_ARGUMENT);
  }
 } FINALLY {
  application_decref(app);
 }
 return result;
}

DEFINE_SYSCALL5(mount,
                USER UNCHECKED char const *,dev_name,
                USER UNCHECKED char const *,dir_name,
                USER UNCHECKED char const *,type,unsigned int,flags,
                USER UNCHECKED void const *,data) {
 REF struct path *p,*dev_path;
 REF struct inode *dev_node;
 if (flags & ~(MS_RDONLY|MS_NOSUID|MS_NODEV|MS_NOEXEC|
               MS_SYNCHRONOUS|MS_REMOUNT|MS_MANDLOCK|
               MS_DIRSYNC|MS_NOATIME|MS_NODIRATIME|
               MS_BIND|MS_MOVE|MS_REC|MS_SILENT|MS_POSIXACL|
               MS_UNBINDABLE|MS_PRIVATE|MS_SLAVE|MS_SHARED|
               MS_RELATIME|MS_I_VERSION|MS_STRICTATIME|
               MS_LAZYTIME|MS_ACTIVE|MS_NOUSER))
     error_throw(E_INVALID_ARGUMENT);

 p = fs_path(NULL,dir_name,user_strlen(dir_name),
             NULL,FS_DEFAULT_ATMODE);
 TRY {
  if (flags & MS_BIND) {
   /* Create a virtual, secondary binding of another, existing mounting point. */
   dev_path = fs_path(NULL,dev_name,user_strlen(dev_name),
                      &dev_node,FS_DEFAULT_ATMODE);
   path_decref(dev_path);
  } else {
   REF struct superblock *block;
   if (!type) {
    REF struct block_device *inode_device;
    /* Automatically determine how to mount some superblock. */
    dev_path = fs_path(NULL,dev_name,user_strlen(dev_name),
                       &dev_node,FS_DEFAULT_ATMODE);
    path_decref(dev_path);
    TRY {
     if (!S_ISBLK(dev_node->i_attr.a_mode))
          error_throw(E_INVALID_ARGUMENT); /* TODO: Loopback devices. */
     inode_device = lookup_block_device(dev_node->i_attr.a_rdev);
    } FINALLY {
     inode_decref(dev_node);
    }
    TRY {
     block = superblock_open(inode_device,NULL,(char *)data);
    } FINALLY {
     block_device_decref(inode_device);
    }
   } else {
    REF struct superblock_type *fstype;
    fstype = lookup_filesystem_type(type);
    TRY {
     if (fstype->st_flags & SUPERBLOCK_TYPE_FSINGLE) {
      block = fstype->st_singleton;
      superblock_incref(block);
     } else if (fstype->st_flags & SUPERBLOCK_TYPE_FNODEV) {
      block = superblock_open(&null_device,fstype,(char *)data);
     } else {
      REF struct block_device *inode_device;
      dev_path = fs_path(NULL,dev_name,user_strlen(dev_name),
                        &dev_node,FS_DEFAULT_ATMODE);
      path_decref(dev_path);
      TRY {
       if (!S_ISBLK(dev_node->i_attr.a_mode))
            error_throw(E_INVALID_ARGUMENT); /* TODO: Loopback devices. */
       /* Lookup the pointed-to block device. */
       inode_device = lookup_block_device(dev_node->i_attr.a_rdev);
      } FINALLY {
       inode_decref(dev_node);
      }
      TRY {
       block = superblock_open(inode_device,fstype,(char *)data);
      } FINALLY {
       block_device_decref(inode_device);
      }
     }
    } FINALLY {
     driver_decref(fstype->st_driver);
    }
   }
   if (block->s_refcnt == 1) {
    if (flags & (MS_NOATIME|MS_NODIRATIME))
        block->s_flags &= ~SUPERBLOCK_FDOATIME;
   }
   /* Mount the root node of the newly created superblock. */
   dev_node = &block->s_root->d_node;
   inode_incref(dev_node);
   superblock_decref(block);
  }
  TRY {
   /* Mount the device node. */
   path_mount(p,dev_node);
  } FINALLY {
   inode_decref(dev_node);
  }
 } FINALLY {
  path_decref(p);
 }
 return 0;
}

DEFINE_SYSCALL2(umount2,
                USER UNCHECKED char const *,name,
                int,flags) {
 atflag_t atflags; REF struct path *p;
 if (flags & ~(MNT_FORCE|MNT_DETACH|MNT_EXPIRE|UMOUNT_NOFOLLOW))
     error_throw(E_INVALID_ARGUMENT);
 atflags = 0;
 if (UMOUNT_NOFOLLOW) atflags |= AT_SYMLINK_NOFOLLOW;
 p = fs_path(NULL,name,user_strlen(name),NULL,FS_ATMODE(atflags));
 if (!path_umount(p))
      throw_fs_error(ERROR_FS_UNMOUNT_NOTAMOUNT);
 return 0;
}

/* TODO */
#define __NR_faccessat    48
#define __NR_nanosleep    101
#define __NR_gettimeofday 169
#define __NR_settimeofday 170


/* Extended system calls (added by KOS). */
/*
#define __NR_xvirtinfo    0x8000002a
INTDEF size_t LIBCCALL Xsys_xdlmodule_info(void *handle, int info_class, void *buf, size_t bufsize);
*/


DECL_END

#endif /* !GUARD_KERNEL_SRC_FS_SYSTEM_C */
