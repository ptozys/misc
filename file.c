static int f2fs_ioc_commit_atomic_file_set(struct file *filp, unsigned long arg)
{
	struct f2fs_sb_info *sbi;
	struct atomic_file_set *afs;
	struct atomic_file *af_elem, *tmp;
	struct inode *inode = NULL;
	struct page *mpage;
	struct list_head *head;
	struct blk_plug plug;
	int ret = 0;
	struct writeback_control wbc = {
		.sync_mode = WB_SYNC_ALL,
		.nr_to_write = LONG_MAX,
		.range_start = 0,
		.range_end = LLONG_MAX,
		.for_reclaim = 0,
	};
	struct inmem_pages *inmem_cur, *inmem_tmp;
	struct inmem_node_pages *inmem_node_cur, *inmem_node_tmp;
	struct f2fs_io_info fio;
	pgoff_t last_idx = ULONG_MAX;
	int count;

	sbi = F2FS_I_SB(filp->f_inode);

	if (!arg) {
		printk("[JATA DBG] (%s) key is NULL\n", __func__);
		return -ENOENT;
	} else {
		read_lock(&sbi->afs_ht_lock);
		afs = (struct atomic_file_set*)rhashtable_lookup_fast(&sbi->afs_ht, &arg,
		                                                      sbi->afs_kht_params);
		read_unlock(&sbi->afs_ht_lock);
	}

	if (!afs) {
		printk("[JATA DBG] %s return -ENOENT because lookup failed\n", __func__);
		return -ENOENT;
	}

	if (!afs->last_file) {
		printk("[JATA DBG] (%s) no last file\n", __func__);
		return 0;
	}

	fio.sbi = sbi;
	fio.type = DATA;
	fio.op = REQ_OP_WRITE;
	fio.op_flags = REQ_SYNC | REQ_PRIO;
	fio.io_type = FS_DATA_IO;
	fio.io_wbc = NULL;

	head = &afs->afs_list;

	/* checkpoint should be blocked before below code block */
	f2fs_balance_fs(sbi, true);
	f2fs_lock_op(sbi);

	/*
	 * Below codes are for processing inmem_pages list
	 * per atomic file group. These code is divided code
	 * of above loop which is for processing inmem_pages
	 * list per inode.
	 * These code is consist of three steps.
	 *
	 * 1. Prepare atomic write of data pages:
	 *    Get the inode lock and other semaphore or
	 *    set the inode flag FI_ATOMIC_COMMIT.
	 * 2. Do atomic write of data pages:
	 *    Walk inmem_pages list in atomic file group
	 *    and make them into one request in plug list.
	 * 3. Release lock
	 * - Joontaek Oh.
	 */

	/*
	 * Step 1: Prepare atomic write of data pages
	 */
	list_for_each_entry_safe(af_elem, tmp, head, list) {
		struct inode *inode = af_elem->inode;
		struct f2fs_inode_info *fi = F2FS_I(inode);
		inode_lock(inode);
		down_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
		mutex_lock(&fi->inmem_lock);
		set_inode_flag(inode, FI_ATOMIC_COMMIT);
	}

	down_write(&afs->afs_rwsem);

	count = afs->added_count;

	afs->committing = true;
	afs->commit_file_count = 0;

	if (!afs->master_nid) {
		ret = f2fs_build_master_node(afs);
		if (ret) {
			printk("[JATA DBG] %s return error"
			       "because building master node failed\n", __func__);
			return ret;
		}
	}

	mpage = f2fs_get_node_page(sbi, afs->master_nid);

	/*
	 * Step 2: Do atomic write of data pages
	 */
	list_for_each_entry_safe(inmem_cur, inmem_tmp, &afs->inmem_pages_list, list) {
		struct page *page;

		if (inmem_cur->stolen) {
			list_move_tail(&inmem_cur->list, &afs->last_file->revoke_list);
			continue;
		}

		page = inmem_cur->page;

		lock_page(page);

		inode = page->mapping->host;
		fio.ino = inode->i_ino;

		if (page->mapping == inode->i_mapping) {
			set_page_dirty(page);
			f2fs_wait_on_page_writeback(page, DATA, true);
			if (clear_page_dirty_for_io(page)) {
				inode_dec_dirty_pages(inode);
				f2fs_remove_dirty_inode(inode);
			}
retry:
			fio.page = page;
			fio.old_blkaddr = NULL_ADDR;
			fio.encrypted_page = NULL;
			fio.need_lock = LOCK_DONE;
			ret = f2fs_do_write_data_page(&fio);
			if (ret) {
				if (ret == -ENOMEM) {
					congestion_wait(BLK_RW_ASYNC, HZ/50);
					cond_resched();
					goto retry;
				}
				unlock_page(page);
				printk("[JATA DBG] (%s) writing"
				       "a data page failed\n", __func__);
				break;
			}

			last_idx = page->index;
		}
		list_move_tail(&inmem_cur->list, &F2FS_I(inode)->af->revoke_list);
		unlock_page(page);
	}

	/*
	 * This code is for sumitting node also.
	 * But it walk only modified node list (afs->inmem_node_pages).
	 * - Joontaek Oh.
	 */
	list_for_each_entry_safe(inmem_node_cur, inmem_node_tmp, 
	                         &afs->inmem_node_pages_list, list) {
		struct page *page = inmem_node_cur->page;

		lock_page(page);

		set_page_dirty(page);
		f2fs_wait_on_page_writeback(page, NODE, true);

		set_fsync_mark(page, 0);
		set_dentry_mark(page, 0);

		if (!clear_page_dirty_for_io(page)) {
			unlock_page(page);
			continue;
		}

		ret = ____write_node_page(page, false, NULL, &wbc, false, FS_NODE_IO);

		if (ret) {
			unlock_page(page);
			printk("[JATA DBG] (%s) writing a node page failed\n", __func__);
			return -1;
		}

		if (IS_INODE(page)) {
			struct master_node *mn;
			struct node_info ni;
			nid_t nid;

			mn = page_address(mpage);

			nid = nid_of_node(page);
			f2fs_get_node_info(sbi, nid, &ni);

			mn->atm_addrs[afs->commit_file_count++] = ni.blk_addr;
		}

		set_page_private(page, 0);
		ClearPagePrivate(page);
		f2fs_put_page(page, 0);
		list_del(&inmem_node_cur->list);
		kmem_cache_free(inmem_entry_slab, inmem_node_cur);
	}

	f2fs_flush_merged_writes(sbi);
	blk_finish_plug(&plug);

	list_for_each_entry_safe(af_elem, tmp, head, list) {
		struct inode *inode;
		inode = af_elem->inode;
		____revoke_inmem_pages(inode, &af_elem->revoke_list, false, false);
	}

	list_for_each_entry_safe(af_elem, tmp, head, list) {
		struct inode *inode;
		inode = af_elem->inode;
		f2fs_wait_on_node_pages_writeback(sbi, inode->i_ino);
	}

	set_fsync_mark(mpage, 1);
	set_page_dirty(mpage);
	f2fs_wait_on_page_writeback(mpage, NODE, true);

	if (!clear_page_dirty_for_io(mpage)) {
		printk("[JATA DBG] (%s) clear master node page fails\n", __func__);
		goto out;
	}

	if (____write_node_page(mpage, true, NULL, &wbc, false, FS_NODE_IO)) {
		printk("[JATA DBG] (%s) master node page write fails\n", __func__);
		unlock_page(mpage);
	}
	f2fs_wait_on_page_writeback(mpage, NODE, true);

	/*
	 * Step 3: Release lock
	 */
	list_for_each_entry_safe(af_elem, tmp, head, list) {
		struct inode *inode = af_elem->inode;
		struct f2fs_inode_info *fi = F2FS_I(inode);

		clear_inode_flag(inode, FI_ATOMIC_COMMIT);
		clear_inode_flag(inode, FI_ATOMIC_FILE);

		mutex_unlock(&fi->inmem_lock);

		fi->i_gc_failures[GC_FAILURE_ATOMIC] = 0;
		stat_dec_atomic_write(inode);

		up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
		inode_unlock(inode);
	}

out:
	f2fs_put_page(mpage, 0);
	afs->commit_file_count = 0;

	afs->committing = false;
	afs->started = false;

	/* checkpoint should be unblocked now. */
	f2fs_unlock_op(sbi);

	up_write(&afs->afs_rwsem);

	return 0;
}


static int f2fs_do_sync_file(struct file *file, loff_t start, loff_t end,
						int datasync, bool atomic)
{
	struct inode *inode = file->f_mapping->host;
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	nid_t ino = inode->i_ino;
	int ret = 0;
	enum cp_reason_type cp_reason = 0;
	struct writeback_control wbc = {
		.sync_mode = WB_SYNC_ALL,
		.nr_to_write = LONG_MAX,
		.for_reclaim = 0,
	};

	if (unlikely(f2fs_readonly(inode->i_sb)))
		return 0;

	trace_f2fs_sync_file_enter(inode);

	/* If file is atomic file, then it should not be synced before tx committing it. */
	if (f2fs_is_atomic_file(inode))
		return 0;

	/* if fdatasync is triggered, let's do in-place-update */
	if (datasync || get_dirty_pages(inode) <= SM_I(sbi)->min_fsync_blocks)
		set_inode_flag(inode, FI_NEED_IPU);
	ret = file_write_and_wait_range(file, start, end);
	clear_inode_flag(inode, FI_NEED_IPU);

	if (ret) {
		trace_f2fs_sync_file_exit(inode, cp_reason, datasync, ret);
		return ret;
	}

	/* if the inode is dirty, let's recover all the time */
	if (!f2fs_skip_inode_update(inode, datasync)) {
		f2fs_write_inode(inode, NULL);
		goto go_write;
	}

	/*
	 * if there is no written data, don't waste time to write recovery info.
	 */
	if (!is_inode_flag_set(inode, FI_APPEND_WRITE) &&
			!f2fs_exist_written_data(sbi, ino, APPEND_INO)) {

		/* it may call write_inode just prior to fsync */
		if (need_inode_page_update(sbi, ino))
			goto go_write;

		if (is_inode_flag_set(inode, FI_UPDATE_WRITE) ||
				f2fs_exist_written_data(sbi, ino, UPDATE_INO))
			goto flush_out;
		goto out;
	}
go_write:
	/*
	 * Both of fdatasync() and fsync() are able to be recovered from
	 * sudden-power-off.
	 */
	down_read(&F2FS_I(inode)->i_sem);
	cp_reason = need_do_checkpoint(inode);
	up_read(&F2FS_I(inode)->i_sem);

	if (cp_reason) {
		/* all the dirty node pages should be flushed for POR */
		ret = f2fs_sync_fs(inode->i_sb, 1);

		/*
		 * We've secured consistency through sync_fs. Following pino
		 * will be used only for fsynced inodes after checkpoint.
		 */
		try_to_fix_pino(inode);
		clear_inode_flag(inode, FI_APPEND_WRITE);
		clear_inode_flag(inode, FI_UPDATE_WRITE);
		goto out;
	}
sync_nodes:
	atomic_inc(&sbi->wb_sync_req[NODE]);
	ret = f2fs_fsync_node_pages(sbi, inode, &wbc, atomic);
	atomic_dec(&sbi->wb_sync_req[NODE]);
	if (ret)
		goto out;

	/* if cp_error was enabled, we should avoid infinite loop */
	if (unlikely(f2fs_cp_error(sbi))) {
		ret = -EIO;
		goto out;
	}

	if (f2fs_need_inode_block_update(sbi, ino) && !f2fs_is_added_file(inode)) {
		f2fs_mark_inode_dirty_sync(inode, true);
		f2fs_write_inode(inode, NULL);
		goto sync_nodes;
	}

	/*
	 * If it's atomic_write, it's just fine to keep write ordering. So
	 * here we don't need to wait for node write completion, since we use
	 * node chain which serializes node blocks. If one of node writes are
	 * reordered, we can see simply broken chain, resulting in stopping
	 * roll-forward recovery. It means we'll recover all or none node blocks
	 * given fsync mark.
	 */
	if (!atomic) {
		ret = f2fs_wait_on_node_pages_writeback(sbi, ino);
		if (ret)
			goto out;
	}

	/* once recovery info is written, don't need to tack this */
	f2fs_remove_ino_entry(sbi, ino, APPEND_INO);
	clear_inode_flag(inode, FI_APPEND_WRITE);
flush_out:
	if (!atomic && F2FS_OPTION(sbi).fsync_mode != FSYNC_MODE_NOBARRIER)
		ret = f2fs_issue_flush(sbi, inode->i_ino);
	if (!ret) {
		f2fs_remove_ino_entry(sbi, ino, UPDATE_INO);
		clear_inode_flag(inode, FI_UPDATE_WRITE);
		f2fs_remove_ino_entry(sbi, ino, FLUSH_INO);
	}
	f2fs_update_time(sbi, REQ_TIME);
out:
	trace_f2fs_sync_file_exit(inode, cp_reason, datasync, ret);
	f2fs_trace_ios(NULL, 1);
	return ret;
}

int file_write_and_wait_range(struct file *file, loff_t lstart, loff_t lend)
{
    int err = 0, err2;
    struct address_space *mapping = file->f_mapping;

    if (mapping_needs_writeback(mapping)) {
        err = __filemap_fdatawrite_range(mapping, lstart, lend,
                         WB_SYNC_ALL);
        /* See comment of filemap_write_and_wait() */
        if (err != -EIO)
            __filemap_fdatawait_range(mapping, lstart, lend);
    }
    err2 = file_check_and_advance_wb_err(file);
    if (!err)
        err = err2;
    return err;
}

int __filemap_fdatawrite_range(struct address_space *mapping, loff_t start,
                loff_t end, int sync_mode)
{
    int ret;
    struct writeback_control wbc = {
        .sync_mode = sync_mode,
        .nr_to_write = LONG_MAX,
        .range_start = start,
        .range_end = end,
    };

    if (!mapping_cap_writeback_dirty(mapping))
        return 0;

    wbc_attach_fdatawrite_inode(&wbc, mapping->host);
    ret = do_writepages(mapping, &wbc);
    wbc_detach_inode(&wbc);
    return ret;
}


int do_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
    int ret;

    if (wbc->nr_to_write <= 0)
        return 0;
    while (1) {
        if (mapping->a_ops->writepages)
            ret = mapping->a_ops->writepages(mapping, wbc);
        else
            ret = generic_writepages(mapping, wbc);
        if ((ret != -ENOMEM) || (wbc->sync_mode != WB_SYNC_ALL))
            break;
        cond_resched();
        congestion_wait(BLK_RW_ASYNC, HZ/50);
    }
    return ret;
}


nt f2fs_sync_fs(struct super_block *sb, int sync)
{
    struct f2fs_sb_info *sbi = F2FS_SB(sb);
    int err = 0;

    if (unlikely(f2fs_cp_error(sbi)))
        return 0;

    trace_f2fs_sync_fs(sb, sync);

    if (unlikely(is_sbi_flag_set(sbi, SBI_POR_DOING)))
        return -EAGAIN;

    if (sync) {
        struct cp_control cpc;

        cpc.reason = __get_cp_reason(sbi);

        mutex_lock(&sbi->gc_mutex);
        err = f2fs_write_checkpoint(sbi, &cpc);
        mutex_unlock(&sbi->gc_mutex);
    }
    f2fs_trace_ios(NULL, 1);

    return err;
}

int f2fs_write_checkpoint(struct f2fs_sb_info *sbi, struct cp_control *cpc)
{   
    struct f2fs_checkpoint *ckpt = F2FS_CKPT(sbi);
    unsigned long long ckpt_ver;
    int err = 0;

    mutex_lock(&sbi->cp_mutex);

    if (!is_sbi_flag_set(sbi, SBI_IS_DIRTY) &&
        ((cpc->reason & CP_FASTBOOT) || (cpc->reason & CP_SYNC) ||
        ((cpc->reason & CP_DISCARD) && !sbi->discard_blks)))
        goto out;
    if (unlikely(f2fs_cp_error(sbi))) {
        err = -EIO;
        goto out;
    }
    if (f2fs_readonly(sbi->sb)) {
        err = -EROFS;
        goto out;
    }

    trace_f2fs_write_checkpoint(sbi->sb, cpc->reason, "start block_ops");

    err = block_operations(sbi);
    if (err)
        goto out;

    trace_f2fs_write_checkpoint(sbi->sb, cpc->reason, "finish block_ops");

    f2fs_flush_merged_writes(sbi);

    /* this is the case of multiple fstrims without any changes */
    if (cpc->reason & CP_DISCARD) {
        if (!f2fs_exist_trim_candidates(sbi, cpc)) {
            unblock_operations(sbi);
            goto out;
        }

        if (NM_I(sbi)->dirty_nat_cnt == 0 &&
                SIT_I(sbi)->dirty_sentries == 0 &&
                prefree_segments(sbi) == 0) {
            f2fs_flush_sit_entries(sbi, cpc);
            f2fs_clear_prefree_segments(sbi, cpc);
            unblock_operations(sbi);
            goto out;
        }
    }

    /*
     * update checkpoint pack index
     * Increase the version number so that
     * SIT entries and seg summaries are written at correct place
     */
    ckpt_ver = cur_cp_version(ckpt);
    ckpt->checkpoint_ver = cpu_to_le64(++ckpt_ver);

    /* write cached NAT/SIT entries to NAT/SIT area */
    f2fs_flush_nat_entries(sbi, cpc);
    f2fs_flush_sit_entries(sbi, cpc);

    /* unlock all the fs_lock[] in do_checkpoint() */
    err = do_checkpoint(sbi, cpc);
    if (err)
        f2fs_release_discard_addrs(sbi);
    else

        f2fs_clear_prefree_segments(sbi, cpc);

    unblock_operations(sbi);
    stat_inc_cp_count(sbi->stat_info);

    if (cpc->reason & CP_RECOVERY)
        f2fs_msg(sbi->sb, KERN_NOTICE,
            "checkpoint: version = %llx", ckpt_ver);

    /* do checkpoint periodically */
    f2fs_update_time(sbi, CP_TIME);
    trace_f2fs_write_checkpoint(sbi->sb, cpc->reason, "finish checkpoint");
out:
    mutex_unlock(&sbi->cp_mutex);
    return err;
}
int f2fs_fsync_node_pages(struct f2fs_sb_info *sbi, struct inode *inode,
            struct writeback_control *wbc, bool atomic)
{       
    pgoff_t index;
    pgoff_t last_idx = ULONG_MAX;
    struct pagevec pvec;
    int ret = 0;
    struct page *last_page = NULL;
    bool marked = false;
    nid_t ino = inode->i_ino;
    int nr_pages;

    if (atomic) {
        last_page = last_fsync_dnode(sbi, ino);
        if (IS_ERR_OR_NULL(last_page))
            return PTR_ERR_OR_ZERO(last_page);
        if (f2fs_is_added_file(inode))
            atomic = false;
    }
retry:
    pagevec_init(&pvec);
    index = 0;


    while ((nr_pages = pagevec_lookup_tag(&pvec, NODE_MAPPING(sbi), &index,
                PAGECACHE_TAG_DIRTY))) {
        int i;

        for (i = 0; i < nr_pages; i++) {
            struct page *page = pvec.pages[i];
            bool submitted = false;

            if (unlikely(f2fs_cp_error(sbi))) {
                f2fs_put_page(last_page, 0);
                pagevec_release(&pvec);
                ret = -EIO;
                goto out;
            }

            if (!IS_DNODE(page) || !is_cold_node(page))
                continue;
            if (ino_of_node(page) != ino)
                continue;
            if (is_master_node(page)) {
                marked = true;
                continue;
            }

            lock_page(page);

            if (unlikely(page->mapping != NODE_MAPPING(sbi))) {
continue_unlock:
                unlock_page(page);
                continue;
            }
            if (ino_of_node(page) != ino)
                goto continue_unlock;

            if (!PageDirty(page) && page != last_page) {
                /* someone wrote it for us */
                goto continue_unlock;
            }

            f2fs_wait_on_page_writeback(page, NODE, true);
            BUG_ON(PageWriteback(page));

            set_fsync_mark(page, 0);
            set_dentry_mark(page, 0);

            if ((!atomic || page == last_page)) {
                set_fsync_mark(page, 1);
                if (IS_INODE(page)) {
                    if (is_inode_flag_set(inode,
                               FI_DIRTY_INODE))
                        f2fs_update_inode(inode, page);
                    set_dentry_mark(page,
                        f2fs_need_dentry_mark(sbi, ino));
                }
                /*  may be written by other thread */
                if (!PageDirty(page))
                    set_page_dirty(page);
            }

            if (!clear_page_dirty_for_io(page))
                goto continue_unlock;

            ret = __write_node_page(page, atomic &&
                        page == last_page,
                        &submitted, wbc, true,
                        FS_NODE_IO);
            if (ret) {
                unlock_page(page);
                f2fs_put_page(last_page, 0);
                break;
            } else if (submitted) {
                last_idx = page->index;
            }

            if (page == last_page) {
                if (f2fs_is_added_file(inode)) {
                    struct atomic_file_set *afs =
                                               F2FS_I(inode)->af->afs;
                    struct master_node *mn;
                    struct page *mpage;
                    struct node_info ni;
                    nid_t nid;

                    mpage = f2fs_get_node_page(sbi, afs->master_nid);
                    mn = page_address(mpage);

                    nid = nid_of_node(page);
                    f2fs_get_node_info(sbi, nid, &ni);

                    mn->atm_addrs[afs->commit_file_count++] =
                                                          ni.blk_addr;
                    f2fs_put_page(mpage, 1);
                }

                f2fs_put_page(page, 0);


                marked = true;
                break;
            }
        }
        pagevec_release(&pvec);
        cond_resched();

        if (ret || marked)
            break;
    }
    if (!ret && atomic && !marked) {
        f2fs_msg(sbi->sb, KERN_DEBUG,
            "Retry to write fsync mark: ino=%u, idx=%lx",
                    ino, last_page->index);
        lock_page(last_page);
        f2fs_wait_on_page_writeback(last_page, NODE, true);
        set_page_dirty(last_page);
        unlock_page(last_page);
        goto retry;
    }
out:
    if (last_idx != ULONG_MAX)
        f2fs_submit_merged_write_cond(sbi, NULL, ino, last_idx, NODE);
    return ret ? -EIO: 0;
}
int f2fs_wait_on_node_pages_writeback(struct f2fs_sb_info *sbi, nid_t ino)
{
    pgoff_t index = 0;
    struct pagevec pvec;
    int ret2, ret = 0;
    int nr_pages;

    pagevec_init(&pvec);

    while ((nr_pages = pagevec_lookup_tag(&pvec, NODE_MAPPING(sbi), &index,
                PAGECACHE_TAG_WRITEBACK))) {
        int i;

        for (i = 0; i < nr_pages; i++) {
            struct page *page = pvec.pages[i];

            if (ino && ino_of_node(page) == ino) {
                f2fs_wait_on_page_writeback(page, NODE, true);
                if (TestClearPageError(page))
                    ret = -EIO;
            }
        }
        pagevec_release(&pvec);
        cond_resched();
    }

    ret2 = filemap_check_errors(NODE_MAPPING(sbi));
    if (!ret)
        ret = ret2;
    return ret;
}
int f2fs_issue_flush(struct f2fs_sb_info *sbi, nid_t ino)
{
    struct flush_cmd_control *fcc = SM_I(sbi)->fcc_info;
    struct flush_cmd cmd;
    int ret;
    
    if (test_opt(sbi, NOBARRIER))
        return 0;
    
    if (!test_opt(sbi, FLUSH_MERGE)) {
        ret = submit_flush_wait(sbi, ino);
        atomic_inc(&fcc->issued_flush);
        return ret;
    }

    if (atomic_inc_return(&fcc->issing_flush) == 1 || sbi->s_ndevs > 1) {
        ret = submit_flush_wait(sbi, ino);
        atomic_dec(&fcc->issing_flush);

        atomic_inc(&fcc->issued_flush);
        return ret;
    }

    cmd.ino = ino;
    init_completion(&cmd.wait);

    llist_add(&cmd.llnode, &fcc->issue_list);

    /* update issue_list before we wake up issue_flush thread */
    smp_mb();

    if (waitqueue_active(&fcc->flush_wait_queue))
        wake_up(&fcc->flush_wait_queue);

    if (fcc->f2fs_issue_flush) {
        wait_for_completion(&cmd.wait);
        atomic_dec(&fcc->issing_flush);
    } else {
        struct llist_node *list;

        list = llist_del_all(&fcc->issue_list);
        if (!list) {
            wait_for_completion(&cmd.wait);
            atomic_dec(&fcc->issing_flush);
        } else {
            struct flush_cmd *tmp, *next;

            ret = submit_flush_wait(sbi, ino);

            llist_for_each_entry_safe(tmp, next, list, llnode) {
                if (tmp == &cmd) {
                    cmd.ret = ret;
                    atomic_dec(&fcc->issing_flush);
                    continue;
                }
                tmp->ret = ret;
                complete(&tmp->wait);
            }
        }
    }

    return cmd.ret;
}
void f2fs_wait_on_page_writeback(struct page *page,
                enum page_type type, bool ordered)
{
    if (PageWriteback(page)) {
        struct f2fs_sb_info *sbi = F2FS_P_SB(page);

        f2fs_submit_merged_write_cond(sbi, page->mapping->host,
                        0, page->index, type);

        if (ordered)
            wait_on_page_writeback(page);
        else
            wait_for_stable_page(page);
    }
}
int f2fs_do_write_data_page(struct f2fs_io_info *fio)
{
    struct page *page = fio->page;
    struct inode *inode = page->mapping->host;
    struct dnode_of_data dn;
    struct extent_info ei = {0,0,0};
    bool ipu_force = false;
    int err = 0;

    set_new_dnode(&dn, inode, NULL, NULL, 0);
    if (need_inplace_update(fio) &&
            f2fs_lookup_extent_cache(inode, page->index, &ei)) {
        fio->old_blkaddr = ei.blk + page->index - ei.fofs;
 
        if (is_valid_blkaddr(fio->old_blkaddr)) {
            ipu_force = true;
            fio->need_lock = LOCK_DONE;
            goto got_it;
        }
    }

    /* Deadlock due to between page->lock and f2fs_lock_op */
    if (fio->need_lock == LOCK_REQ && !f2fs_trylock_op(fio->sbi))
        return -EAGAIN;

    err = f2fs_get_dnode_of_data(&dn, page->index, LOOKUP_NODE);
    if (err)
        goto out;

    fio->old_blkaddr = dn.data_blkaddr;

    /* This page is already truncated */
    if (fio->old_blkaddr == NULL_ADDR) {
        ClearPageUptodate(page);
        goto out_writepage;
    }
got_it:
    /*
     * If current allocation needs SSR,
     * it had better in-place writes for updated data.
     */
    if (ipu_force || (is_valid_blkaddr(fio->old_blkaddr) &&
                    need_inplace_update(fio))) {
        err = encrypt_one_page(fio);
        if (err)
            goto out_writepage;

        set_page_writeback(page);
        ClearPageError(page);
        f2fs_put_dnode(&dn);
        if (fio->need_lock == LOCK_REQ)
            f2fs_unlock_op(fio->sbi);
        err = f2fs_inplace_write_data(fio);
        trace_f2fs_do_write_data_page(fio->page, IPU);
        set_inode_flag(inode, FI_UPDATE_WRITE);
        return err;
    }
    if (fio->need_lock == LOCK_RETRY) {
        if (!f2fs_trylock_op(fio->sbi)) {
            err = -EAGAIN;
            goto out_writepage;
        }
        fio->need_lock = LOCK_REQ;
    }

    err = encrypt_one_page(fio);
    if (err)
        goto out_writepage;

    set_page_writeback(page);
    ClearPageError(page);

    /* LFS mode write path */
    f2fs_outplace_write_data(&dn, fio);
    trace_f2fs_do_write_data_page(page, OPU);
    set_inode_flag(inode, FI_APPEND_WRITE);
    if (page->index == 0)
        set_inode_flag(inode, FI_FIRST_BLOCK_WRITTEN);
out_writepage:
    f2fs_put_dnode(&dn);
out:
    if (fio->need_lock == LOCK_REQ)
        f2fs_unlock_op(fio->sbi);
    return err;
}
int ____write_node_page(struct page *page, bool atomic, bool *submitted,
                struct writeback_control *wbc, bool do_balance,
                enum iostat_type io_type)
{
    return __write_node_page(page, atomic, submitted, wbc, do_balance, io_type);
}
static int __write_node_page(struct page *page, bool atomic, bool *submitted,
                struct writeback_control *wbc, bool do_balance,
                enum iostat_type io_type)
{
    struct f2fs_sb_info *sbi = F2FS_P_SB(page);
    nid_t nid;
    struct node_info ni;
    struct f2fs_io_info fio = {
        .sbi = sbi,
        .ino = ino_of_node(page),
        .type = NODE,
        .op = REQ_OP_WRITE,
        .op_flags = wbc_to_write_flags(wbc),
        .page = page,
        .encrypted_page = NULL,
        .submitted = false,
        .io_type = io_type,
        .io_wbc = wbc,
    };

    trace_f2fs_writepage(page, NODE);
    if (unlikely(f2fs_cp_error(sbi)))
        goto redirty_out;

    if (unlikely(is_sbi_flag_set(sbi, SBI_POR_DOING)))
        goto redirty_out;

    /* get old block addr of this node page */
    nid = nid_of_node(page);
    f2fs_bug_on(sbi, page->index != nid);

    if (wbc->for_reclaim) {
        if (!down_read_trylock(&sbi->node_write))
            goto redirty_out;
    } else {
        down_read(&sbi->node_write);
    }

    f2fs_get_node_info(sbi, nid, &ni);

    /* This page is already truncated */
    if (unlikely(ni.blk_addr == NULL_ADDR)) {
        ClearPageUptodate(page);
        dec_page_count(sbi, F2FS_DIRTY_NODES);
        up_read(&sbi->node_write);
        unlock_page(page);
        return 0;
    }

    if (atomic && !test_opt(sbi, NOBARRIER))
        fio.op_flags |= REQ_PREFLUSH | REQ_FUA;

    set_page_writeback(page);
    ClearPageError(page);
    fio.old_blkaddr = ni.blk_addr;
    f2fs_do_write_node_page(nid, &fio);
    set_node_addr(sbi, &ni, fio.new_blkaddr, is_fsync_dnode(page));
    dec_page_count(sbi, F2FS_DIRTY_NODES);
    up_read(&sbi->node_write);

    if (wbc->for_reclaim) {
        f2fs_submit_merged_write_cond(sbi, page->mapping->host, 0,
                        page->index, NODE);
        submitted = NULL;
    }

    unlock_page(page);

    if (unlikely(f2fs_cp_error(sbi))) {
        f2fs_submit_merged_write(sbi, NODE);
        submitted = NULL;
    }
    if (submitted)
        *submitted = fio.submitted;

    if (do_balance)
        f2fs_balance_fs(sbi, false);
    return 0;

redirty_out:
    redirty_page_for_writepage(wbc, page);
    return AOP_WRITEPAGE_ACTIVATE;
}

void f2fs_flush_merged_writes(struct f2fs_sb_info *sbi)
{
    f2fs_submit_merged_write(sbi, DATA);
    f2fs_submit_merged_write(sbi, NODE);
    f2fs_submit_merged_write(sbi, META);
}
void f2fs_submit_merged_write(struct f2fs_sb_info *sbi, enum page_type type)
{
    __submit_merged_write_cond(sbi, NULL, 0, 0, type, true);
}
static void __submit_merged_write_cond(struct f2fs_sb_info *sbi,
                struct inode *inode, nid_t ino, pgoff_t idx,
                enum page_type type, bool force)
{
    enum temp_type temp;

    if (!force && !has_merged_page(sbi, inode, ino, idx, type))
        return;

    for (temp = HOT; temp < NR_TEMP_TYPE; temp++) {

        __f2fs_submit_merged_write(sbi, type, temp);

        /* TODO: use HOT temp only for meta pages now. */
        if (type >= META)
            break;
    }
}
static int __revoke_inmem_pages(struct inode *inode,
                struct list_head *head, bool drop, bool recover)
{
    struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
    struct inmem_pages *cur, *tmp;
    int err = 0;

    list_for_each_entry_safe(cur, tmp, head, list) {
        struct page *page = cur->page;

        if (cur->stolen) {
            list_del(&cur->list);
            kmem_cache_free(inmem_entry_slab, cur);
            continue;
        }
        inode = page->mapping->host;

        if (drop)
            trace_f2fs_commit_inmem_page(page, INMEM_DROP);

        lock_page(page);

        f2fs_wait_on_page_writeback(page, DATA, true);

        if (recover) {
            struct dnode_of_data dn;
            struct node_info ni;

            trace_f2fs_commit_inmem_page(page, INMEM_REVOKE);
retry:
            set_new_dnode(&dn, inode, NULL, NULL, 0);
            err = f2fs_get_dnode_of_data(&dn, page->index,
                                LOOKUP_NODE);
            if (err) {
                if (err == -ENOMEM) {
                    congestion_wait(BLK_RW_ASYNC, HZ/50);
                    cond_resched();
                    goto retry;
                }
                err = -EAGAIN;
                goto next;
            }
            f2fs_get_node_info(sbi, dn.nid, &ni);
            if (cur->old_addr == NEW_ADDR) {
                f2fs_invalidate_blocks(sbi, dn.data_blkaddr);
                f2fs_update_data_blkaddr(&dn, NEW_ADDR);
            } else
                f2fs_replace_block(sbi, &dn, dn.data_blkaddr,
                    cur->old_addr, ni.version, true, true);
            f2fs_put_dnode(&dn);
        }
next:
        /* we don't need to invalidate this in the sccessful status */
        if (drop || recover)
            ClearPageUptodate(page);
        set_page_private(page, 0);
        ClearPagePrivate(page);

        unlock_page(page);

        list_del(&cur->list);
        kmem_cache_free(inmem_entry_slab, cur);
        dec_page_count(F2FS_I_SB(inode), F2FS_INMEM_PAGES);
    }
    return err;
}

int ____revoke_inmem_pages(struct inode *inode,
                struct list_head *head, bool drop, bool recover)
{
    return __revoke_inmem_pages(inode, head, drop, recover);
}

