/*
 * fs/inotify_user.c - inotify support for userspace
 *
 * Authors:
 *	John McCutchan	<ttb@tentacle.dhs.org>
 *	Robert Love	<rml@novell.com>
 *
 * Copyright (C) 2005 John McCutchan
 * Copyright 2006 Hewlett-Packard Development Company, L.P.
 *
 * Copyright (C) 2009 Eric Paris <Red Hat Inc>
 * inotify was largely rewriten to make use of the fsnotify infrastructure
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/file.h>
#include <linux/fs.h> /* struct inode */
#include <linux/fsnotify_backend.h>
#include <linux/idr.h>
#include <linux/init.h> /* module_init */
#include <linux/inotify.h>
#include <linux/kernel.h> /* roundup() */
#include <linux/namei.h> /* LOOKUP_FOLLOW */
#include <linux/sched.h> /* struct user */
#include <linux/slab.h> /* struct kmem_cache */
#include <linux/syscalls.h>
#include <linux/types.h>
#include <linux/anon_inodes.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/wait.h>

#include "inotify.h"
#include "../fdinfo.h"

#include <asm/ioctls.h>


#include <asm/hibernate.h>
#ifdef HIBERNATE_ANDROID_MODE

#include <linux/fdtable.h>
#include <asm/errno.h>
#include <asm/atomic.h>

#endif

/* these are configurable via /proc/sys/fs/inotify/ */
static int inotify_max_user_instances __read_mostly;
static int inotify_max_queued_events __read_mostly;
static int inotify_max_user_watches __read_mostly;

static struct kmem_cache *inotify_inode_mark_cachep __read_mostly;
struct kmem_cache *event_priv_cachep __read_mostly;

#ifdef CONFIG_SYSCTL

#include <linux/sysctl.h>

static int zero;

#ifdef HIBERNATE_ANDROID_MODE

static bool hibernate_mode = false;
static LIST_HEAD(hibernate_inotify_list);

#define HIBERNATE_UMOUNT_RW_NUM (sizeof(hibernate_umount_rw) / sizeof(char *))

static const char *hibernate_umount_rw[] = {
	HIBERNATE_UMOUNT_RW
};

struct hibernate_inotify {
	struct list_head list;
	struct task_struct* task;
	pid_t pid;
	int group_no;
	int inotify_fd;
	int inode_based_fd;
	int inode_now_fd;
	u32 mask;
	char pathname[1023+2];
	struct fsnotify_group *group;
};

#endif

ctl_table inotify_table[] = {
	{
		.procname	= "max_user_instances",
		.data		= &inotify_max_user_instances,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &zero,
	},
	{
		.procname	= "max_user_watches",
		.data		= &inotify_max_user_watches,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &zero,
	},
	{
		.procname	= "max_queued_events",
		.data		= &inotify_max_queued_events,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &zero
	},
	{ }
};
#endif /* CONFIG_SYSCTL */


#ifdef HIBERNATE_ANDROID_MODE

#ifdef __HIBERNATE_DEBUG_LOG
#define HIBERNATE_DEBUG_PRINTK(fmt, ...) printk(KERN_INFO "%s(%d):"fmt, __FUNCTION__, __LINE__, ## __VA_ARGS__)
#else
#define HIBERNATE_DEBUG_PRINTK(fmt, ...)  
#endif

#else

#define HIBERNATE_DEBUG_PRINTK(fmt, ...)  

#endif

static inline __u32 inotify_arg_to_mask(u32 arg)
{
	__u32 mask;

	HIBERNATE_DEBUG_PRINTK("inotify: Call inotify_arg_to_mask(arg: [%d])\n", arg);

	/*
	 * everything should accept their own ignored, cares about children,
	 * and should receive events when the inode is unmounted
	 */
	mask = (FS_IN_IGNORED | FS_EVENT_ON_CHILD | FS_UNMOUNT);

	/* mask off the flags used to open the fd */
	mask |= (arg & (IN_ALL_EVENTS | IN_ONESHOT | IN_EXCL_UNLINK));

	HIBERNATE_DEBUG_PRINTK("inotify: arg: [%x] -> mask: [%x])\n", arg, mask);

	return mask;
}

static inline u32 inotify_mask_to_arg(__u32 mask)
{
	return mask & (IN_ALL_EVENTS | IN_ISDIR | IN_UNMOUNT | IN_IGNORED |
		       IN_Q_OVERFLOW);
}

/* intofiy userspace file descriptor functions */
static unsigned int inotify_poll(struct file *file, poll_table *wait)
{
	struct fsnotify_group *group = file->private_data;
	int ret = 0;

	poll_wait(file, &group->notification_waitq, wait);
	mutex_lock(&group->notification_mutex);
	if (!fsnotify_notify_queue_is_empty(group))
		ret = POLLIN | POLLRDNORM;
	mutex_unlock(&group->notification_mutex);

	return ret;
}

/*
 * Get an inotify_kernel_event if one exists and is small
 * enough to fit in "count". Return an error pointer if
 * not large enough.
 *
 * Called with the group->notification_mutex held.
 */
static struct fsnotify_event *get_one_event(struct fsnotify_group *group,
					    size_t count)
{
	size_t event_size = sizeof(struct inotify_event);
	struct fsnotify_event *event;

	if (fsnotify_notify_queue_is_empty(group))
		return NULL;

	event = fsnotify_peek_notify_event(group);

	pr_debug("%s: group=%p event=%p\n", __func__, group, event);

	if (event->name_len)
		event_size += roundup(event->name_len + 1, event_size);

	if (event_size > count)
		return ERR_PTR(-EINVAL);

	/* held the notification_mutex the whole time, so this is the
	 * same event we peeked above */
	fsnotify_remove_notify_event(group);

	return event;
}

/*
 * Copy an event to user space, returning how much we copied.
 *
 * We already checked that the event size is smaller than the
 * buffer we had in "get_one_event()" above.
 */
static ssize_t copy_event_to_user(struct fsnotify_group *group,
				  struct fsnotify_event *event,
				  char __user *buf)
{
	struct inotify_event inotify_event;
	struct fsnotify_event_private_data *fsn_priv;
	struct inotify_event_private_data *priv;
	size_t event_size = sizeof(struct inotify_event);
	size_t name_len = 0;

	pr_debug("%s: group=%p event=%p\n", __func__, group, event);

#ifdef HIBERNATE_ANDROID_MODE
	HIBERNATE_DEBUG_PRINTK("group=%p event=%p\n", group, event);
	
	struct list_head *p;
	struct hibernate_inotify *myobj;
#endif
	
	/* we get the inotify watch descriptor from the event private data */
	spin_lock(&event->lock);
	fsn_priv = fsnotify_remove_priv_from_event(group, event);
	spin_unlock(&event->lock);

	if (!fsn_priv)
		inotify_event.wd = -1;
	else {
		priv = container_of(fsn_priv, struct inotify_event_private_data,
				    fsnotify_event_priv_data);
				    
#ifdef HIBERNATE_ANDROID_MODE
			
		int wd = priv->wd;
		list_for_each(p, &hibernate_inotify_list) {
			myobj = list_entry(p, struct hibernate_inotify, list);

			HIBERNATE_DEBUG_PRINTK("group=%p:%p wd=%p:%p\n"
								, myobj->group, group, myobj->inode_now_fd, priv->wd);

			if (myobj->group == group && myobj->inode_now_fd == priv->wd) {
				priv->wd = myobj->inode_based_fd;
				HIBERNATE_DEBUG_PRINTK("Bingo! wd:[%d]->[%d]", wd, priv->wd);
				break;
			}
		}
		if (wd != priv->wd) {
			HIBERNATE_DEBUG_PRINTK("no items. wd=[%d]", priv->wd);
		}
#endif
		inotify_event.wd = priv->wd;
		inotify_free_event_priv(fsn_priv);
	}

	/*
	 * round up event->name_len so it is a multiple of event_size
	 * plus an extra byte for the terminating '\0'.
	 */
	if (event->name_len)
		name_len = roundup(event->name_len + 1, event_size);
	inotify_event.len = name_len;

	inotify_event.mask = inotify_mask_to_arg(event->mask);
	inotify_event.cookie = event->sync_cookie;

	/* send the main event */
#ifdef HIBERNATE_ANDROID_MODE
	if (copy_to_user(buf, &inotify_event, event_size)) {
		HIBERNATE_DEBUG_PRINTK("copy_event_to_user: -EFAULT(1)\n");
		return -EFAULT;
	}
#else
	if (copy_to_user(buf, &inotify_event, event_size))
		return -EFAULT;
#endif
	buf += event_size;

	/*
	 * fsnotify only stores the pathname, so here we have to send the pathname
	 * and then pad that pathname out to a multiple of sizeof(inotify_event)
	 * with zeros.  I get my zeros from the nul_inotify_event.
	 */
	if (name_len) {
		unsigned int len_to_zero = name_len - event->name_len;
		/* copy the path name */
#ifdef HIBERNATE_ANDROID_MODE
		if (copy_to_user(buf, event->file_name, event->name_len)) {
			HIBERNATE_DEBUG_PRINTK("copy_event_to_user: -EFAULT(2)\n");
			return -EFAULT;
		}
#else
		if (copy_to_user(buf, event->file_name, event->name_len))
			return -EFAULT;
#endif
		buf += event->name_len;

		/* fill userspace with 0's */
		if (clear_user(buf, len_to_zero))
			return -EFAULT;
		buf += len_to_zero;
		event_size += name_len;
	}

	return event_size;
}

static ssize_t inotify_read(struct file *file, char __user *buf,
			    size_t count, loff_t *pos)
{
	struct fsnotify_group *group;
	struct fsnotify_event *kevent;
	char __user *start;
	int ret;
	DEFINE_WAIT(wait);

	start = buf;
	group = file->private_data;

	while (1) {
		prepare_to_wait(&group->notification_waitq, &wait, TASK_INTERRUPTIBLE);

		mutex_lock(&group->notification_mutex);
		kevent = get_one_event(group, count);
		mutex_unlock(&group->notification_mutex);

		pr_debug("%s: group=%p kevent=%p\n", __func__, group, kevent);

		if (kevent) {
			ret = PTR_ERR(kevent);
			if (IS_ERR(kevent))
				break;
			ret = copy_event_to_user(group, kevent, buf);
			fsnotify_put_event(kevent);
			if (ret < 0)
				break;
			buf += ret;
			count -= ret;
			continue;
		}

		ret = -EAGAIN;
		if (file->f_flags & O_NONBLOCK)
			break;
		ret = -ERESTARTSYS;
		if (signal_pending(current))
			break;

		if (start != buf)
			break;

		schedule();
	}

	finish_wait(&group->notification_waitq, &wait);
	if (start != buf && ret != -EFAULT)
		ret = buf - start;
	return ret;
}

static int inotify_release(struct inode *ignored, struct file *file)
{
	struct fsnotify_group *group = file->private_data;

	pr_debug("%s: group=%p\n", __func__, group);

	/* free this group, matching get was inotify_init->fsnotify_obtain_group */
	fsnotify_destroy_group(group);

	return 0;
}

static long inotify_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct fsnotify_group *group;
	struct fsnotify_event_holder *holder;
	struct fsnotify_event *event;
	void __user *p;
	int ret = -ENOTTY;
	size_t send_len = 0;

	group = file->private_data;
	p = (void __user *) arg;

	pr_debug("%s: group=%p cmd=%u\n", __func__, group, cmd);

	switch (cmd) {
	case FIONREAD:
		mutex_lock(&group->notification_mutex);
		list_for_each_entry(holder, &group->notification_list, event_list) {
			event = holder->event;
			send_len += sizeof(struct inotify_event);
			if (event->name_len)
				send_len += roundup(event->name_len + 1,
						sizeof(struct inotify_event));
		}
		mutex_unlock(&group->notification_mutex);
		ret = put_user(send_len, (int __user *) p);
		break;
	}

	return ret;
}

static const struct file_operations inotify_fops = {
	.show_fdinfo	= inotify_show_fdinfo,
	.poll		= inotify_poll,
	.read		= inotify_read,
	.fasync		= fsnotify_fasync,
	.release	= inotify_release,
	.unlocked_ioctl	= inotify_ioctl,
	.compat_ioctl	= inotify_ioctl,
	.llseek		= noop_llseek,
};


/*
 * find_inode - resolve a user-given path to a specific inode
 */
static int inotify_find_inode(const char __user *dirname, struct path *path, unsigned flags)
{
	int error;
	
#ifdef HIBERNATE_ANDROID_MODE

	HIBERNATE_DEBUG_PRINTK("inotify_find_inode: __user(%s)\n", dirname);
	HIBERNATE_DEBUG_PRINTK("inotify_find_inode: path(%s)\n", error);
	HIBERNATE_DEBUG_PRINTK("inotify_find_inode: flags(%d)\n", flags);

	error = user_path_at(AT_FDCWD, dirname, flags, path);
	if (error) {
		HIBERNATE_DEBUG_PRINTK("inotify_find_inode: user_path_at(error :[%d])\n", error);
		return error;
	}
	/* you can only watch an inode if you have read permissions on it */
	error = inode_permission(path->dentry->d_inode, MAY_READ);
	if (error) {
		HIBERNATE_DEBUG_PRINTK("inotify_find_inode: inode_permission(error :[%d])\n", error);
		path_put(path);
	}
	HIBERNATE_DEBUG_PRINTK("inotify_find_inode: return(error :[%d])\n", error);
	
#else

	error = user_path_at(AT_FDCWD, dirname, flags, path);
	if (error)
		return error;
	/* you can only watch an inode if you have read permissions on it */
	error = inode_permission(path->dentry->d_inode, MAY_READ);
	if (error)
		path_put(path);
#endif

	return error;
}

static int inotify_add_to_idr(struct idr *idr, spinlock_t *idr_lock,
			      struct inotify_inode_mark *i_mark)
{
	int ret;

	idr_preload(GFP_KERNEL);
	spin_lock(idr_lock);

	ret = idr_alloc_cyclic(idr, i_mark, 1, 0, GFP_NOWAIT);
	if (ret >= 0) {
		/* we added the mark to the idr, take a reference */
		i_mark->wd = ret;
		fsnotify_get_mark(&i_mark->fsn_mark);
	}

	spin_unlock(idr_lock);
	idr_preload_end();
	return ret < 0 ? ret : 0;
}

static struct inotify_inode_mark *inotify_idr_find_locked(struct fsnotify_group *group,
								int wd)
{
	struct idr *idr = &group->inotify_data.idr;
	spinlock_t *idr_lock = &group->inotify_data.idr_lock;
	struct inotify_inode_mark *i_mark;

	assert_spin_locked(idr_lock);

	i_mark = idr_find(idr, wd);
	if (i_mark) {
		struct fsnotify_mark *fsn_mark = &i_mark->fsn_mark;

		fsnotify_get_mark(fsn_mark);
		/* One ref for being in the idr, one ref we just took */
		BUG_ON(atomic_read(&fsn_mark->refcnt) < 2);
	}

	return i_mark;
}

static struct inotify_inode_mark *inotify_idr_find(struct fsnotify_group *group,
							 int wd)
{
	struct inotify_inode_mark *i_mark;
	spinlock_t *idr_lock = &group->inotify_data.idr_lock;

	spin_lock(idr_lock);
	i_mark = inotify_idr_find_locked(group, wd);
	spin_unlock(idr_lock);

	return i_mark;
}

static void do_inotify_remove_from_idr(struct fsnotify_group *group,
				       struct inotify_inode_mark *i_mark)
{
	struct idr *idr = &group->inotify_data.idr;
	spinlock_t *idr_lock = &group->inotify_data.idr_lock;
	int wd = i_mark->wd;

	assert_spin_locked(idr_lock);

	idr_remove(idr, wd);

	/* removed from the idr, drop that ref */
	fsnotify_put_mark(&i_mark->fsn_mark);
}

/*
 * Remove the mark from the idr (if present) and drop the reference
 * on the mark because it was in the idr.
 */
static void inotify_remove_from_idr(struct fsnotify_group *group,
				    struct inotify_inode_mark *i_mark)
{
	spinlock_t *idr_lock = &group->inotify_data.idr_lock;
	struct inotify_inode_mark *found_i_mark = NULL;
	int wd;

	spin_lock(idr_lock);
	wd = i_mark->wd;

	/*
	 * does this i_mark think it is in the idr?  we shouldn't get called
	 * if it wasn't....
	 */
	if (wd == -1) {
		WARN_ONCE(1, "%s: i_mark=%p i_mark->wd=%d i_mark->group=%p"
			" i_mark->inode=%p\n", __func__, i_mark, i_mark->wd,
			i_mark->fsn_mark.group, i_mark->fsn_mark.i.inode);
		goto out;
	}

	/* Lets look in the idr to see if we find it */
	found_i_mark = inotify_idr_find_locked(group, wd);
	if (unlikely(!found_i_mark)) {
		WARN_ONCE(1, "%s: i_mark=%p i_mark->wd=%d i_mark->group=%p"
			" i_mark->inode=%p\n", __func__, i_mark, i_mark->wd,
			i_mark->fsn_mark.group, i_mark->fsn_mark.i.inode);
		goto out;
	}

	/*
	 * We found an mark in the idr at the right wd, but it's
	 * not the mark we were told to remove.  eparis seriously
	 * fucked up somewhere.
	 */
	if (unlikely(found_i_mark != i_mark)) {
		WARN_ONCE(1, "%s: i_mark=%p i_mark->wd=%d i_mark->group=%p "
			"mark->inode=%p found_i_mark=%p found_i_mark->wd=%d "
			"found_i_mark->group=%p found_i_mark->inode=%p\n",
			__func__, i_mark, i_mark->wd, i_mark->fsn_mark.group,
			i_mark->fsn_mark.i.inode, found_i_mark, found_i_mark->wd,
			found_i_mark->fsn_mark.group,
			found_i_mark->fsn_mark.i.inode);
		goto out;
	}

	/*
	 * One ref for being in the idr
	 * one ref held by the caller trying to kill us
	 * one ref grabbed by inotify_idr_find
	 */
	if (unlikely(atomic_read(&i_mark->fsn_mark.refcnt) < 3)) {
		printk(KERN_ERR "%s: i_mark=%p i_mark->wd=%d i_mark->group=%p"
			" i_mark->inode=%p\n", __func__, i_mark, i_mark->wd,
			i_mark->fsn_mark.group, i_mark->fsn_mark.i.inode);
		/* we can't really recover with bad ref cnting.. */
		BUG();
	}

	do_inotify_remove_from_idr(group, i_mark);
out:
	/* match the ref taken by inotify_idr_find_locked() */
	if (found_i_mark)
		fsnotify_put_mark(&found_i_mark->fsn_mark);
	i_mark->wd = -1;
	spin_unlock(idr_lock);
}

/*
 * Send IN_IGNORED for this wd, remove this wd from the idr.
 */
void inotify_ignored_and_remove_idr(struct fsnotify_mark *fsn_mark,
				    struct fsnotify_group *group)
{
	struct inotify_inode_mark *i_mark;
	struct fsnotify_event *ignored_event, *notify_event;
	struct inotify_event_private_data *event_priv;
	struct fsnotify_event_private_data *fsn_event_priv;
	int ret;

	i_mark = container_of(fsn_mark, struct inotify_inode_mark, fsn_mark);

	ignored_event = fsnotify_create_event(NULL, FS_IN_IGNORED, NULL,
					      FSNOTIFY_EVENT_NONE, NULL, 0,
					      GFP_NOFS);
	if (!ignored_event)
		goto skip_send_ignore;

	event_priv = kmem_cache_alloc(event_priv_cachep, GFP_NOFS);
	if (unlikely(!event_priv))
		goto skip_send_ignore;

	fsn_event_priv = &event_priv->fsnotify_event_priv_data;

	fsnotify_get_group(group);
	fsn_event_priv->group = group;
	event_priv->wd = i_mark->wd;

	notify_event = fsnotify_add_notify_event(group, ignored_event, fsn_event_priv, NULL);
	if (notify_event) {
		if (IS_ERR(notify_event))
			ret = PTR_ERR(notify_event);
		else
			fsnotify_put_event(notify_event);
		inotify_free_event_priv(fsn_event_priv);
	}

skip_send_ignore:
	/* matches the reference taken when the event was created */
	if (ignored_event)
		fsnotify_put_event(ignored_event);

	/* remove this mark from the idr */
	inotify_remove_from_idr(group, i_mark);

	atomic_dec(&group->inotify_data.user->inotify_watches);
}

/* ding dong the mark is dead */
static void inotify_free_mark(struct fsnotify_mark *fsn_mark)
{
	struct inotify_inode_mark *i_mark;

	i_mark = container_of(fsn_mark, struct inotify_inode_mark, fsn_mark);

	kmem_cache_free(inotify_inode_mark_cachep, i_mark);
}

static int inotify_update_existing_watch(struct fsnotify_group *group,
					 struct inode *inode,
					 u32 arg)
{
	struct fsnotify_mark *fsn_mark;
	struct inotify_inode_mark *i_mark;
	__u32 old_mask, new_mask;
	__u32 mask;
	int add = (arg & IN_MASK_ADD);
	int ret;

	mask = inotify_arg_to_mask(arg);

	fsn_mark = fsnotify_find_inode_mark(group, inode);
	if (!fsn_mark)
		return -ENOENT;

	i_mark = container_of(fsn_mark, struct inotify_inode_mark, fsn_mark);

	spin_lock(&fsn_mark->lock);

	old_mask = fsn_mark->mask;
	if (add)
		fsnotify_set_mark_mask_locked(fsn_mark, (fsn_mark->mask | mask));
	else
		fsnotify_set_mark_mask_locked(fsn_mark, mask);
	new_mask = fsn_mark->mask;

	spin_unlock(&fsn_mark->lock);

	if (old_mask != new_mask) {
		/* more bits in old than in new? */
		int dropped = (old_mask & ~new_mask);
		/* more bits in this fsn_mark than the inode's mask? */
		int do_inode = (new_mask & ~inode->i_fsnotify_mask);

		/* update the inode with this new fsn_mark */
		if (dropped || do_inode)
			fsnotify_recalc_inode_mask(inode);

	}

	/* return the wd */
	ret = i_mark->wd;

	/* match the get from fsnotify_find_mark() */
	fsnotify_put_mark(fsn_mark);

	return ret;
}

static int inotify_new_watch(struct fsnotify_group *group,
			     struct inode *inode,
			     u32 arg)
{
	struct inotify_inode_mark *tmp_i_mark;
	__u32 mask;
	int ret;
	struct idr *idr = &group->inotify_data.idr;
	spinlock_t *idr_lock = &group->inotify_data.idr_lock;

	mask = inotify_arg_to_mask(arg);

	tmp_i_mark = kmem_cache_alloc(inotify_inode_mark_cachep, GFP_KERNEL);
	if (unlikely(!tmp_i_mark))
		return -ENOMEM;

	fsnotify_init_mark(&tmp_i_mark->fsn_mark, inotify_free_mark);
	tmp_i_mark->fsn_mark.mask = mask;
	tmp_i_mark->wd = -1;

	ret = -ENOSPC;
	if (atomic_read(&group->inotify_data.user->inotify_watches) >= inotify_max_user_watches)
		goto out_err;

	ret = inotify_add_to_idr(idr, idr_lock, tmp_i_mark);
	if (ret)
		goto out_err;

	/* we are on the idr, now get on the inode */
	ret = fsnotify_add_mark(&tmp_i_mark->fsn_mark, group, inode, NULL, 0);
	if (ret) {
		/* we failed to get on the inode, get off the idr */
		inotify_remove_from_idr(group, tmp_i_mark);
		goto out_err;
	}

	/* increment the number of watches the user has */
	atomic_inc(&group->inotify_data.user->inotify_watches);

	/* return the watch descriptor for this new mark */
	ret = tmp_i_mark->wd;

out_err:
	/* match the ref from fsnotify_init_mark() */
	fsnotify_put_mark(&tmp_i_mark->fsn_mark);

	return ret;
}

static int inotify_update_watch(struct fsnotify_group *group, struct inode *inode, u32 arg)
{
	int ret = 0;

retry:
	/* try to update and existing watch with the new arg */
	ret = inotify_update_existing_watch(group, inode, arg);
	/* no mark present, try to add a new one */
	if (ret == -ENOENT)
		ret = inotify_new_watch(group, inode, arg);
	/*
	 * inotify_new_watch could race with another thread which did an
	 * inotify_new_watch between the update_existing and the add watch
	 * here, go back and try to update an existing mark again.
	 */
	if (ret == -EEXIST)
		goto retry;

	return ret;
}

static struct fsnotify_group *inotify_new_group(unsigned int max_events)
{
	struct fsnotify_group *group;

	group = fsnotify_alloc_group(&inotify_fsnotify_ops);
	if (IS_ERR(group))
		return group;

	group->max_events = max_events;

	spin_lock_init(&group->inotify_data.idr_lock);
	idr_init(&group->inotify_data.idr);
	group->inotify_data.user = get_current_user();

	if (atomic_inc_return(&group->inotify_data.user->inotify_devs) >
	    inotify_max_user_instances) {
		fsnotify_destroy_group(group);
		return ERR_PTR(-EMFILE);
	}

	return group;
}


/* inotify syscalls */
SYSCALL_DEFINE1(inotify_init1, int, flags)
{
	struct fsnotify_group *group;
	int ret;

	/* Check the IN_* constants for consistency.  */
	BUILD_BUG_ON(IN_CLOEXEC != O_CLOEXEC);
	BUILD_BUG_ON(IN_NONBLOCK != O_NONBLOCK);

	if (flags & ~(IN_CLOEXEC | IN_NONBLOCK))
		return -EINVAL;

	/* fsnotify_obtain_group took a reference to group, we put this when we kill the file in the end */
	group = inotify_new_group(inotify_max_queued_events);
	if (IS_ERR(group))
		return PTR_ERR(group);

	ret = anon_inode_getfd("inotify", &inotify_fops, group,
				  O_RDONLY | flags);
	if (ret < 0)
		fsnotify_destroy_group(group);

	return ret;
}

SYSCALL_DEFINE0(inotify_init)
{
	return sys_inotify_init1(0);
}

#ifdef HIBERNATE_ANDROID_MODE

/*
* file_table.c 
*/
struct file *hibernate_fget_light(unsigned int fd, int *fput_needed, struct task_struct* _task)
{
	struct file *file;
	struct files_struct *files = _task->files;
	int count = atomic_read(&files->count);
	*fput_needed = 0;
	if (likely(count == 1)) {
		file = fcheck_files(files, fd);
	} else {
		rcu_read_lock();
		file = fcheck_files(files, fd);
		if (file) {
			if (atomic_long_inc_not_zero(&file->f_count)) {
				*fput_needed = 1;
			} else {
				/* Didn't get the reference, someone's freed */
				file = NULL;
			}
		}

		rcu_read_unlock();
	}

	return file;
}

int hibernate_inotify_add_watch(
	int fd,
	const char __user * pathname,
	u32 mask,
	bool add_fg,
	struct task_struct* _task
){
	struct fsnotify_group *group;
	struct inode *inode;
	struct path path;
	struct file *filp;
	int ret, fput_needed;
	unsigned flags = 0;

	bool check_fg = false;

	struct hibernate_inotify *myobj;
	struct list_head *p;
	int group_no;
	mm_segment_t fs;

	filp = hibernate_fget_light(fd, &fput_needed, _task);
	if (unlikely(!filp)) {
		HIBERNATE_DEBUG_PRINTK("-hibernate_inotify_add_watch: hibernate_fget_light=-EBADF\n");
		return -EBADF;
	}

	if (unlikely(filp->f_op != &inotify_fops)) {
		ret = -EINVAL;
		goto fput_and_out;
	}

	if (!(mask & IN_DONT_FOLLOW))
		flags |= LOOKUP_FOLLOW;
	if (mask & IN_ONLYDIR)
		flags |= LOOKUP_DIRECTORY;

	HIBERNATE_DEBUG_PRINTK("inotify: Before inotify_find_inode(fd:[%d])\n", fd);

	fs = get_fs();
	set_fs(KERNEL_DS);
	ret = inotify_find_inode(pathname, &path, flags);
	set_fs(fs);

	HIBERNATE_DEBUG_PRINTK("-hibernate_inotify_add_watch: ret=[%d]\n", ret);

	if (ret) {
		list_for_each(p, &hibernate_inotify_list) {
			myobj = list_entry(p, struct hibernate_inotify, list);
			int strcmp_ret = strcmp(pathname, myobj->pathname);
			if (myobj->pid == _task->pid && myobj->inotify_fd == fd && myobj->mask == mask && 
				strcmp(pathname, myobj->pathname) == 0) {
				myobj->inode_now_fd = ret;
			}
		}
		HIBERNATE_DEBUG_PRINTK("-hibernate_inotify_add_watch: ret=(%d)\n", ret);
		goto fput_and_out;
	}

	inode = path.dentry->d_inode;
	group = filp->private_data;

	ret = inotify_update_watch(group, inode, mask);
	path_put(&path);

	group_no = atomic_read(&group->refcnt);

	int no;
	for (no = 0; no < HIBERNATE_UMOUNT_RW_NUM; no++) {
		int po = strncmp(hibernate_umount_rw[no], pathname, strlen(hibernate_umount_rw[no]));
		if (po == 0) {
			check_fg = true;
		}
	}
	
	HIBERNATE_DEBUG_PRINTK("-inotify_find_inode: check_fg=(%d)\n", check_fg);

	if (add_fg && check_fg) {
		myobj = kmalloc(sizeof(struct hibernate_inotify), GFP_KERNEL);
		memset(myobj, 0, sizeof(struct hibernate_inotify));

		myobj->task = current;
		myobj->pid = current->pid;
		myobj->inotify_fd = fd;
		myobj->group_no = group_no;
		myobj->inode_now_fd = ret;
		myobj->inode_based_fd = ret;
		myobj->mask = mask;
		strncpy(myobj->pathname, pathname, strlen(pathname));
		myobj->group = group;

		list_add_tail(&myobj->list, &hibernate_inotify_list);
	} else {
		list_for_each(p, &hibernate_inotify_list) {
			myobj = list_entry(p, struct hibernate_inotify, list);
			int strcmp_ret = strcmp(pathname, myobj->pathname);
			if (myobj->inotify_fd == fd && myobj->mask == mask && 
				strcmp(pathname, myobj->pathname) == 0) {
				myobj->inode_now_fd = ret;
			}
		}
	}

#ifdef __HIBERNATE_DEBUG_DUMP
	printk( KERN_INFO "*** %s [%d] parent[%d] %s\n", _task->comm, _task->pid, _task->parent->pid, _task->parent->comm );

	list_for_each(p, &hibernate_inotify_list) {
		myobj = list_entry(p, struct hibernate_inotify, list);

		HIBERNATE_DEBUG_PRINTK("-inotify: list_for_each pid(%d) fd:inotify[%d]-group[%p][%d]-base[%d]-now[%d] mask:[%d] pathname:[%s] \n"
		, myobj->pid , myobj->inotify_fd, myobj->group, myobj->group_no, myobj->inode_based_fd, myobj->inode_now_fd
		, myobj->mask, myobj->pathname);
	}
#endif

fput_and_out:
	fput_light(filp, fput_needed);
	return ret;
}


SYSCALL_DEFINE3(inotify_add_watch, int, fd, const char __user *, pathname,
		u32, mask)
{
	HIBERNATE_DEBUG_PRINTK("inotify: Call hibernate_inotify_add_watch(fd:[%d], pathname:[%s], mask:[%d])\n",
		fd, pathname, mask);
	return hibernate_inotify_add_watch(fd, pathname, mask, true, current);
}

int inotify_hibernate_restart(void) {
	HIBERNATE_DEBUG_PRINTK("+inotify: Call inotify_hibernate_restart()\n");

	struct hibernate_inotify *myobj;
	struct list_head *p;

	int i = 0;

	list_for_each(p, &hibernate_inotify_list) {
		myobj = list_entry(p, struct hibernate_inotify, list);

		int inotify_fd = myobj->inotify_fd;
		int inode_based_fd = myobj->inode_based_fd;
		int inode_now_fd = myobj->inode_now_fd;

		char* pathname = myobj->pathname;
		u32 mask = myobj->mask;

		myobj->inode_now_fd = hibernate_inotify_add_watch(inotify_fd, pathname, mask, false, myobj->task);

		HIBERNATE_DEBUG_PRINTK("+inotify:[%02d] list_for_each inode_now_fd old[%d] -> new[%d]\n", i, inode_now_fd, myobj->inode_now_fd);

		HIBERNATE_DEBUG_PRINTK("+inotify:[%02d] list_for_each fd:inotify[%d]-base[%d]-now[%d] mask:[%d] pathname:[%s] \n"
		, i, myobj->inotify_fd, myobj->inode_based_fd, myobj->inode_now_fd
		, myobj->mask, myobj->pathname);
	}

	hibernate_mode = false;
	return 0;
}



int hibernate_check_pid(void) {

	HIBERNATE_DEBUG_PRINTK("*inotify: Call hibernate_check_pid()\n");

	struct hibernate_inotify *myobj;
	struct list_head *p;

	list_for_each(p, &hibernate_inotify_list) {

		myobj = list_entry(p, struct hibernate_inotify, list);
		struct task_struct* _task = myobj->task;

		printk( KERN_INFO "* %s [%d] state(%ld)\n", _task->comm, _task->pid, _task->state);

		if (myobj->pid != _task->pid) {
			HIBERNATE_DEBUG_PRINTK("*myobj->pid(%d) != _task->pid(%d) \n", myobj->pid, _task->pid);
			list_del(myobj);
			hibernate_check_pid();
			HIBERNATE_DEBUG_PRINTK("*break\n");
			break;
		}

		if (_task->state < 0) {
			HIBERNATE_DEBUG_PRINTK("*sask->state(%ld)\n", _task->state);
			hibernate_check_pid();
			HIBERNATE_DEBUG_PRINTK("*break\n");
			break;
		}		
	}
	HIBERNATE_DEBUG_PRINTK("*inotify: EXIT hibernate_check_pid()\n");

}

int inotify_hibernate_wait(void) {
	HIBERNATE_DEBUG_PRINTK("inotify: Call inotify_hibernate_wait()\n");
	hibernate_mode = true;

	struct hibernate_inotify *myobj;
	struct list_head *p;

	hibernate_check_pid();

	list_for_each(p, &hibernate_inotify_list) {
		myobj = list_entry(p, struct hibernate_inotify, list);
		HIBERNATE_DEBUG_PRINTK("inotify_hibernate_wait: list_for_each fd:inotify[%d]-base[%d]-now[%d] mask:[%d] pathname:[%s] \n"
			, myobj->inotify_fd, myobj->inode_based_fd, myobj->inode_now_fd
			, myobj->mask, myobj->pathname);

		struct task_struct* _task = myobj->task;
		struct files_struct *_files = _task->files;


		if (myobj->pid != _task->pid) {
			HIBERNATE_DEBUG_PRINTK("myobj->pid(%d) != _task->pid(%d) \n", myobj->pid, _task->pid);
			HIBERNATE_DEBUG_PRINTK("continue\n");
			continue;
		}

		if (_task->state < 0) {
			HIBERNATE_DEBUG_PRINTK("sask->state(%ld)\n", _task->state);
			HIBERNATE_DEBUG_PRINTK("continue\n");
			continue;
		}

		struct fsnotify_group *group;
		struct inotify_inode_mark *i_mark;
		struct file *filp;
		int ret = 0, fput_needed;
		int inotify_fd = myobj->inotify_fd;

		filp = hibernate_fget_light(inotify_fd, &fput_needed, _task);
		if (unlikely(!filp)) {
			HIBERNATE_DEBUG_PRINTK("hibernate_fget_light:return -EBADF\n");
			HIBERNATE_DEBUG_PRINTK("hibernate_fget_light:continue\n");
			continue;
		}

		/* verify that this is indeed an inotify instance */
		ret = -EINVAL;

		if (unlikely(filp->f_op != &inotify_fops)) {
			HIBERNATE_DEBUG_PRINTK("return -EBADF\n");
			HIBERNATE_DEBUG_PRINTK("continue\n");
			goto hibernate_out;
		}

		group = filp->private_data;

		ret = -EINVAL;
		i_mark = inotify_idr_find(group, myobj->inode_now_fd);
		if (unlikely(!i_mark)) {
			HIBERNATE_DEBUG_PRINTK("inotify_idr_find:return -EBADF; goto hibernate_out\n");
			goto hibernate_out;
		}

		ret = 0;

		fsnotify_destroy_mark(&i_mark->fsn_mark, group);
		/* match ref taken by inotify_idr_find */
		fsnotify_put_mark(&i_mark->fsn_mark);
hibernate_out:
		fput_light(filp, fput_needed);

	}

	return 0;
}

#else

SYSCALL_DEFINE3(inotify_add_watch, int, fd, const char __user *, pathname,
		u32, mask)
{
	struct fsnotify_group *group;
	struct inode *inode;
	struct path path;
	struct fd f;
	int ret;
	unsigned flags = 0;

	HIBERNATE_DEBUG_PRINTK("inotify: Call inotify_add_watch()\n");

	/* don't allow invalid bits: we don't want flags set */
	if (unlikely(!(mask & ALL_INOTIFY_BITS)))
		return -EINVAL;

	f = fdget(fd);
	if (unlikely(!f.file))
		return -EBADF;

	/* verify that this is indeed an inotify instance */
	if (unlikely(f.file->f_op != &inotify_fops)) {
		ret = -EINVAL;
		goto fput_and_out;
	}

	if (!(mask & IN_DONT_FOLLOW))
		flags |= LOOKUP_FOLLOW;
	if (mask & IN_ONLYDIR)
		flags |= LOOKUP_DIRECTORY;

	ret = inotify_find_inode(pathname, &path, flags);
	if (ret)
		goto fput_and_out;

	/* inode held in place by reference to path; group by fget on fd */
	inode = path.dentry->d_inode;
	group = f.file->private_data;

	/* create/update an inode mark */
	ret = inotify_update_watch(group, inode, mask);
	path_put(&path);
fput_and_out:
	fdput(f);
	return ret;
}
#endif

SYSCALL_DEFINE2(inotify_rm_watch, int, fd, __s32, wd)
{
	struct fsnotify_group *group;
	struct inotify_inode_mark *i_mark;
	struct fd f;
	int ret = 0;
#ifdef HIBERNATE_ANDROID_MODE 
	struct list_head *p; 
	struct hibernate_inotify *myobj; 
#endif 

	f = fdget(fd);
	if (unlikely(!f.file))
		return -EBADF;

	/* verify that this is indeed an inotify instance */
	ret = -EINVAL;
	if (unlikely(f.file->f_op != &inotify_fops))
		goto out;

	group = f.file->private_data;

	ret = -EINVAL;
	i_mark = inotify_idr_find(group, wd);
	if (unlikely(!i_mark))
		goto out;

#ifdef HIBERNATE_ANDROID_MODE
	list_for_each(p, &hibernate_inotify_list) {
		myobj = list_entry(p, struct hibernate_inotify, list);

		if (myobj->task->pid == current->pid && myobj->inode_now_fd == wd) {

			HIBERNATE_DEBUG_PRINTK("inotify: inotify_rm_watch pid(%d) fd:inotify[%d]-group[%p][%d]-base[%d]-now[%d] mask:[%d] pathname:[%s] \n"
			, myobj->task->pid , myobj->inotify_fd, myobj->group, myobj->group_no, myobj->inode_based_fd, myobj->inode_now_fd
			, myobj->mask, myobj->pathname);

			list_del(myobj);
			break;
		}
	}
#endif

	ret = 0;

	fsnotify_destroy_mark(&i_mark->fsn_mark, group);

	/* match ref taken by inotify_idr_find */
	fsnotify_put_mark(&i_mark->fsn_mark);

out:
	fdput(f);
	return ret;
}

/*
 * inotify_user_setup - Our initialization function.  Note that we cannot return
 * error because we have compiled-in VFS hooks.  So an (unlikely) failure here
 * must result in panic().
 */
static int __init inotify_user_setup(void)
{
	BUILD_BUG_ON(IN_ACCESS != FS_ACCESS);
	BUILD_BUG_ON(IN_MODIFY != FS_MODIFY);
	BUILD_BUG_ON(IN_ATTRIB != FS_ATTRIB);
	BUILD_BUG_ON(IN_CLOSE_WRITE != FS_CLOSE_WRITE);
	BUILD_BUG_ON(IN_CLOSE_NOWRITE != FS_CLOSE_NOWRITE);
	BUILD_BUG_ON(IN_OPEN != FS_OPEN);
	BUILD_BUG_ON(IN_MOVED_FROM != FS_MOVED_FROM);
	BUILD_BUG_ON(IN_MOVED_TO != FS_MOVED_TO);
	BUILD_BUG_ON(IN_CREATE != FS_CREATE);
	BUILD_BUG_ON(IN_DELETE != FS_DELETE);
	BUILD_BUG_ON(IN_DELETE_SELF != FS_DELETE_SELF);
	BUILD_BUG_ON(IN_MOVE_SELF != FS_MOVE_SELF);
	BUILD_BUG_ON(IN_UNMOUNT != FS_UNMOUNT);
	BUILD_BUG_ON(IN_Q_OVERFLOW != FS_Q_OVERFLOW);
	BUILD_BUG_ON(IN_IGNORED != FS_IN_IGNORED);
	BUILD_BUG_ON(IN_EXCL_UNLINK != FS_EXCL_UNLINK);
	BUILD_BUG_ON(IN_ISDIR != FS_ISDIR);
	BUILD_BUG_ON(IN_ONESHOT != FS_IN_ONESHOT);

	BUG_ON(hweight32(ALL_INOTIFY_BITS) != 21);

	inotify_inode_mark_cachep = KMEM_CACHE(inotify_inode_mark, SLAB_PANIC);
	event_priv_cachep = KMEM_CACHE(inotify_event_private_data, SLAB_PANIC);

	inotify_max_queued_events = 16384;
	inotify_max_user_instances = 128;
	inotify_max_user_watches = 8192;

	return 0;
}
module_init(inotify_user_setup);
