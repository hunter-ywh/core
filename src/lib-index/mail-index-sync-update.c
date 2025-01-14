/* Copyright (c) 2004-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "mmap-util.h"
#include "mail-index-modseq.h"
#include "mail-index-view-private.h"
#include "mail-index-sync-private.h"
#include "mail-transaction-log.h"
#include "mail-transaction-log-private.h"

/* If we have less than this many bytes to sync from log file, don't bother
   reading the main index */
#define MAIL_INDEX_SYNC_MIN_READ_INDEX_SIZE 2048

static void
mail_index_sync_update_log_offset(struct mail_index_sync_map_ctx *ctx,
				  struct mail_index_map *map, bool eol)
{
	uint32_t prev_seq;
	uoff_t prev_offset;

	mail_transaction_log_view_get_prev_pos(ctx->view->log_view,
					       &prev_seq, &prev_offset);
	if (prev_seq == 0) {
		/* handling lost changes in view syncing */
		return;
	}

	if (!eol) {
		if (prev_offset == ctx->ext_intro_end_offset &&
		    prev_seq == ctx->ext_intro_seq) {
			/* previous transaction was an extension introduction.
			   we probably came here from
			   mail_index_sync_ext_reset(). if there are any more
			   views which want to continue syncing it needs the
			   intro. so back up a bit more.

			   don't do this in case the last transaction in the
			   log is the extension intro, so we don't keep trying
			   to sync it over and over again. */
			prev_offset = ctx->ext_intro_offset;
		}
		map->hdr.log_file_seq = prev_seq;
	} else {
		i_assert(ctx->view->index->log->head->hdr.file_seq == prev_seq);
		if (map->hdr.log_file_seq != prev_seq) {
			map->hdr.log_file_seq = prev_seq;
			map->hdr.log_file_tail_offset = 0;
		}
	}
	map->hdr.log_file_head_offset = prev_offset;
}

static void mail_index_sync_replace_map(struct mail_index_sync_map_ctx *ctx,
					struct mail_index_map *map)
{
        struct mail_index_view *view = ctx->view;

	i_assert(view->map != map);

	mail_index_sync_update_log_offset(ctx, view->map, FALSE);
	mail_index_unmap(&view->map);
	view->map = map;

	if (ctx->type != MAIL_INDEX_SYNC_HANDLER_VIEW)
		view->index->map = map;

	mail_index_modseq_sync_map_replaced(ctx->modseq_ctx);
}

static struct mail_index_map *
mail_index_sync_move_to_private_memory(struct mail_index_sync_map_ctx *ctx)
{
	struct mail_index_map *map = ctx->view->map;

	if (map->refcount > 1) {
		/* Multiple views point to this map. Make a copy of the map
		   (but not rec_map). */
		map = mail_index_map_clone(map);
		mail_index_sync_replace_map(ctx, map);
		i_assert(ctx->view->map == map);
	}

	if (!MAIL_INDEX_MAP_IS_IN_MEMORY(ctx->view->map)) {
		/* map points to mmap()ed area, copy it into memory. */
		mail_index_map_move_to_memory(ctx->view->map);
		mail_index_modseq_sync_map_replaced(ctx->modseq_ctx);
	}
	return map;
}

struct mail_index_map *
mail_index_sync_get_atomic_map(struct mail_index_sync_map_ctx *ctx)
{
	/* First make sure we have a private map with rec_map pointing to
	   memory. */
	(void)mail_index_sync_move_to_private_memory(ctx);
	/* Next make sure the rec_map is also private to us. */
	mail_index_record_map_move_to_private(ctx->view->map);
	mail_index_modseq_sync_map_replaced(ctx->modseq_ctx);
	return ctx->view->map;
}

static int
mail_index_header_update_counts(struct mail_index_header *hdr,
				uint8_t old_flags, uint8_t new_flags,
				const char **error_r)
{
	if (((old_flags ^ new_flags) & MAIL_SEEN) != 0) {
		/* different seen-flag */
		if ((old_flags & MAIL_SEEN) != 0) {
			if (hdr->seen_messages_count == 0) {
				*error_r = "Seen counter wrong";
				return -1;
			}
			hdr->seen_messages_count--;
		} else {
			if (hdr->seen_messages_count >= hdr->messages_count) {
				*error_r = "Seen counter wrong";
				return -1;
			}

			if (++hdr->seen_messages_count == hdr->messages_count)
				hdr->first_unseen_uid_lowwater = hdr->next_uid;
		}
	}

	if (((old_flags ^ new_flags) & MAIL_DELETED) != 0) {
		/* different deleted-flag */
		if ((old_flags & MAIL_DELETED) == 0) {
			hdr->deleted_messages_count++;
			if (hdr->deleted_messages_count > hdr->messages_count) {
				*error_r = "Deleted counter wrong";
				return -1;
			}
		} else {
			if (hdr->deleted_messages_count == 0 ||
			    hdr->deleted_messages_count > hdr->messages_count) {
				*error_r = "Deleted counter wrong";
				return -1;
			}

			if (--hdr->deleted_messages_count == 0)
				hdr->first_deleted_uid_lowwater = hdr->next_uid;
		}
	}
	return 0;
}

static void
mail_index_sync_header_update_counts_all(struct mail_index_sync_map_ctx *ctx,
					 uint32_t uid,
					 uint8_t old_flags, uint8_t new_flags)
{
	struct mail_index_map *const *maps;
	const char *error;
	unsigned int i, count;

	maps = array_get(&ctx->view->map->rec_map->maps, &count);
	for (i = 0; i < count; i++) {
		if (uid >= maps[i]->hdr.next_uid)
			continue;

		if (mail_index_header_update_counts(&maps[i]->hdr,
						    old_flags, new_flags,
						    &error) < 0)
			mail_index_sync_set_corrupted(ctx, "%s", error);
	}
}

static void
mail_index_sync_header_update_counts(struct mail_index_sync_map_ctx *ctx,
				     uint32_t uid, uint8_t old_flags,
				     uint8_t new_flags)
{
	const char *error;

	if (uid >= ctx->view->map->hdr.next_uid) {
		mail_index_sync_set_corrupted(ctx, "uid %u >= next_uid %u",
					      uid, ctx->view->map->hdr.next_uid);
	} else {
		if (mail_index_header_update_counts(&ctx->view->map->hdr,
						    old_flags, new_flags,
						    &error) < 0)
			mail_index_sync_set_corrupted(ctx, "%s", error);
	}
}

static void
mail_index_header_update_lowwaters(struct mail_index_sync_map_ctx *ctx,
				   uint32_t uid, enum mail_flags flags)
{
	struct mail_index_map *const *maps;
	unsigned int i, count;

	maps = array_get(&ctx->view->map->rec_map->maps, &count);
	for (i = 0; i < count; i++) {
		if ((flags & MAIL_SEEN) == 0 &&
		    uid < maps[i]->hdr.first_unseen_uid_lowwater)
			maps[i]->hdr.first_unseen_uid_lowwater = uid;
		if ((flags & MAIL_DELETED) != 0 &&
		    uid < maps[i]->hdr.first_deleted_uid_lowwater)
			maps[i]->hdr.first_deleted_uid_lowwater = uid;
	}
}

static void
sync_expunge_call_handlers(struct mail_index_sync_map_ctx *ctx,
			   uint32_t seq1, uint32_t seq2)
{
	const struct mail_index_expunge_handler *eh;
	struct mail_index_record *rec;
	uint32_t seq;

	array_foreach(&ctx->expunge_handlers, eh) {
		for (seq = seq1; seq <= seq2; seq++) {
			rec = MAIL_INDEX_REC_AT_SEQ(ctx->view->map, seq);
			eh->handler(ctx, PTR_OFFSET(rec, eh->record_offset),
				    eh->sync_context);
		}
	}
}

static bool
sync_expunge_handlers_init(struct mail_index_sync_map_ctx *ctx)
{
	/* call expunge handlers only when syncing index file */
	if (ctx->type != MAIL_INDEX_SYNC_HANDLER_FILE)
		return FALSE;

	if (!ctx->expunge_handlers_set)
		mail_index_sync_init_expunge_handlers(ctx);

	if (!array_is_created(&ctx->expunge_handlers))
		return FALSE;
	return TRUE;
}

static void
sync_expunge_range(struct mail_index_sync_map_ctx *ctx, const ARRAY_TYPE(seq_range) *seqs)
{
	struct mail_index_map *map;
	const struct seq_range *range;
	unsigned int i, count;
	uint32_t dest_seq1, prev_seq2, orig_rec_count;

	range = array_get(seqs, &count);
	if (count == 0)
		return;

	/* Get a private in-memory rec_map, which we can modify. */
	map = mail_index_sync_get_atomic_map(ctx);

	/* call the expunge handlers first */
	if (sync_expunge_handlers_init(ctx)) {
		for (i = 0; i < count; i++) {
			sync_expunge_call_handlers(ctx,
				range[i].seq1, range[i].seq2);
		}
	}

	prev_seq2 = 0;
	dest_seq1 = 1;
	orig_rec_count = map->rec_map->records_count;
	for (i = 0; i < count; i++) {
		uint32_t seq1 = range[i].seq1;
		uint32_t seq2 = range[i].seq2;
		struct mail_index_record *rec;
		uint32_t seq_count, seq;

		i_assert(seq1 > prev_seq2);

		for (seq = seq1; seq <= seq2; seq++) {
			rec = MAIL_INDEX_REC_AT_SEQ(map, seq);
			mail_index_sync_header_update_counts(ctx, rec->uid, rec->flags, 0);
		}

		if (prev_seq2+1 <= seq1-1) {
			/* @UNSAFE: move (prev_seq2+1) .. (seq1-1) to its
			   final location in the map if necessary */
			uint32_t move_count = (seq1-1) - (prev_seq2+1) + 1;
			if (prev_seq2+1-1 != dest_seq1-1)
				memmove(MAIL_INDEX_REC_AT_SEQ(map, dest_seq1),
					MAIL_INDEX_REC_AT_SEQ(map, prev_seq2+1),
					move_count * map->hdr.record_size);
			dest_seq1 += move_count;
		}
		seq_count = seq2 - seq1 + 1;
		map->rec_map->records_count -= seq_count;
		map->hdr.messages_count -= seq_count;
		mail_index_modseq_expunge(ctx->modseq_ctx, seq1, seq2);
		prev_seq2 = seq2;
	}
	/* Final stragglers */
	if (orig_rec_count > prev_seq2) {
		uint32_t final_move_count = orig_rec_count - prev_seq2;
		memmove(MAIL_INDEX_REC_AT_SEQ(map, dest_seq1),
			MAIL_INDEX_REC_AT_SEQ(map, prev_seq2+1),
			final_move_count * map->hdr.record_size);
	}
}

static void *sync_append_record(struct mail_index_map *map)
{
	size_t append_pos;
	void *ret;

	append_pos = map->rec_map->records_count * map->hdr.record_size;
	ret = buffer_get_space_unsafe(map->rec_map->buffer, append_pos,
				      map->hdr.record_size);
	map->rec_map->records =
		buffer_get_modifiable_data(map->rec_map->buffer, NULL);
	return ret;
}

static bool sync_update_ignored_change(struct mail_index_sync_map_ctx *ctx)
{
	struct mail_index_transaction_commit_result *result =
		ctx->view->index->sync_commit_result;
	uint32_t prev_log_seq;
	uoff_t prev_log_offset, trans_start_offset, trans_end_offset;

	if (result == NULL)
		return FALSE;

	/* we'll return TRUE if this modseq change was written within the
	   transaction that was just committed */
	mail_transaction_log_view_get_prev_pos(ctx->view->log_view,
					       &prev_log_seq, &prev_log_offset);
	if (prev_log_seq != result->log_file_seq)
		return FALSE;

	trans_end_offset = result->log_file_offset;
	trans_start_offset = trans_end_offset - result->commit_size;
	if (prev_log_offset < trans_start_offset ||
	    prev_log_offset >= trans_end_offset)
		return FALSE;

	return TRUE;
}

static int
sync_modseq_update(struct mail_index_sync_map_ctx *ctx,
		   const struct mail_transaction_modseq_update *u,
		   unsigned int size)
{
	struct mail_index_view *view = ctx->view;
	const struct mail_transaction_modseq_update *end;
	uint32_t seq;
	uint64_t min_modseq;
	int ret;

	end = CONST_PTR_OFFSET(u, size);
	for (; u < end; u++) {
		if (u->uid == 0)
			seq = 0;
		else if (!mail_index_lookup_seq(view, u->uid, &seq))
			continue;

		min_modseq = ((uint64_t)u->modseq_high32 << 32) |
			u->modseq_low32;

		ret = seq == 0 ? 1 :
			mail_index_modseq_set(view, seq, min_modseq);
		if (ret < 0) {
			mail_index_sync_set_corrupted(ctx,
				"modseqs updated before they were enabled");
			return -1;
		}
		if (ret == 0 && sync_update_ignored_change(ctx))
			view->index->sync_commit_result->ignored_modseq_changes++;
	}
	return 1;
}

static int sync_append(const struct mail_index_record *rec,
		       struct mail_index_sync_map_ctx *ctx)
{
	struct mail_index_view *view = ctx->view;
	struct mail_index_map *map = view->map;
	const struct mail_index_record *old_rec;
	enum mail_flags new_flags;
	void *dest;

	if (rec->uid < map->hdr.next_uid) {
		mail_index_sync_set_corrupted(ctx,
			"Append with UID %u, but next_uid = %u",
			rec->uid, map->hdr.next_uid);
		return -1;
	}

	/* We'll need to append a new record. If map currently points to
	   mmap()ed index, it first needs to be moved to memory since we can't
	   write past the mmap()ed memory area. */
	map = mail_index_sync_move_to_private_memory(ctx);

	if (rec->uid <= map->rec_map->last_appended_uid) {
		i_assert(map->hdr.messages_count < map->rec_map->records_count);
		/* the flags may have changed since it was added to map.
		   use the updated flags already, so flag counters won't get
		   broken. */
		old_rec = MAIL_INDEX_MAP_IDX(map, map->hdr.messages_count);
		i_assert(old_rec->uid == rec->uid);
		new_flags = old_rec->flags;
	} else {
		/* don't rely on buffer->used being at the correct position.
		   at least expunges can move it */
		dest = sync_append_record(map);
		memcpy(dest, rec, sizeof(*rec));
		memset(PTR_OFFSET(dest, sizeof(*rec)), 0,
		       map->hdr.record_size - sizeof(*rec));
		map->rec_map->records_count++;
		map->rec_map->last_appended_uid = rec->uid;
		new_flags = rec->flags;

		mail_index_modseq_append(ctx->modseq_ctx,
					 map->rec_map->records_count);
	}

	map->hdr.messages_count++;
	map->hdr.next_uid = rec->uid+1;

	if ((new_flags & MAIL_INDEX_MAIL_FLAG_DIRTY) != 0 &&
	    (view->index->flags & MAIL_INDEX_OPEN_FLAG_NO_DIRTY) == 0)
		map->hdr.flags |= MAIL_INDEX_HDR_FLAG_HAVE_DIRTY;

	mail_index_header_update_lowwaters(ctx, rec->uid, new_flags);
	mail_index_sync_header_update_counts(ctx, rec->uid, 0, new_flags);
	return 1;
}

static int sync_flag_update(const struct mail_transaction_flag_update *u,
			    struct mail_index_sync_map_ctx *ctx)
{
	struct mail_index_view *view = ctx->view;
	struct mail_index_record *rec;
	uint8_t flag_mask, old_flags;
	uint32_t seq, seq1, seq2;

	if (!mail_index_lookup_seq_range(view, u->uid1, u->uid2, &seq1, &seq2))
		return 1;

	if (!MAIL_TRANSACTION_FLAG_UPDATE_IS_INTERNAL(u)) {
		mail_index_modseq_update_flags(ctx->modseq_ctx,
					       u->add_flags | u->remove_flags,
					       seq1, seq2);
	}

	if ((u->add_flags & MAIL_INDEX_MAIL_FLAG_DIRTY) != 0 &&
	    (view->index->flags & MAIL_INDEX_OPEN_FLAG_NO_DIRTY) == 0)
		view->map->hdr.flags |= MAIL_INDEX_HDR_FLAG_HAVE_DIRTY;

        flag_mask = (unsigned char)~u->remove_flags;

	if (((u->add_flags | u->remove_flags) &
	     (MAIL_SEEN | MAIL_DELETED)) == 0) {
		/* we're not modifying any counted/lowwatered flags */
		for (seq = seq1; seq <= seq2; seq++) {
			rec = MAIL_INDEX_REC_AT_SEQ(view->map, seq);
			rec->flags = (rec->flags & flag_mask) | u->add_flags;
		}
	} else {
		for (seq = seq1; seq <= seq2; seq++) {
			rec = MAIL_INDEX_REC_AT_SEQ(view->map, seq);

			old_flags = rec->flags;
			rec->flags = (rec->flags & flag_mask) | u->add_flags;

			mail_index_header_update_lowwaters(ctx, rec->uid,
							   rec->flags);
			mail_index_sync_header_update_counts_all(ctx, rec->uid,
								 old_flags,
								 rec->flags);
		}
	}
	return 1;
}

static int sync_header_update(const struct mail_transaction_header_update *u,
			      struct mail_index_sync_map_ctx *ctx)
{
	struct mail_index_map *map = ctx->view->map;
	uint32_t orig_log_file_tail_offset = map->hdr.log_file_tail_offset;
	uint32_t orig_next_uid = map->hdr.next_uid;

	if (u->offset >= map->hdr.base_header_size ||
	    u->offset + u->size > map->hdr.base_header_size) {
		mail_index_sync_set_corrupted(ctx,
			"Header update outside range: %u + %u > %u",
			u->offset, u->size, map->hdr.base_header_size);
		return -1;
	}

	buffer_write(map->hdr_copy_buf, u->offset, u + 1, u->size);
	i_assert(map->hdr_copy_buf->used == map->hdr.header_size);

	/* @UNSAFE */
	if ((uint32_t)(u->offset + u->size) <= sizeof(map->hdr)) {
		memcpy(PTR_OFFSET(&map->hdr, u->offset),
		       u + 1, u->size);
	} else if (u->offset < sizeof(map->hdr)) {
		memcpy(PTR_OFFSET(&map->hdr, u->offset),
		       u + 1, sizeof(map->hdr) - u->offset);
	}

	if (map->hdr.next_uid < orig_next_uid) {
		/* next_uid update tried to shrink its value. this can happen
		   in some race conditions with e.g. with dsync, so just
		   silently ignore it. */
		map->hdr.next_uid = orig_next_uid;
	}

	/* the tail offset updates are intended for internal transaction
	   log handling. we'll update the offset in the header only when
	   the sync is finished. */
	map->hdr.log_file_tail_offset = orig_log_file_tail_offset;
	return 1;
}

static int
mail_index_sync_record_real(struct mail_index_sync_map_ctx *ctx,
			    const struct mail_transaction_header *hdr,
			    const void *data)
{
	int ret = 0;

	switch (hdr->type & MAIL_TRANSACTION_TYPE_MASK) {
	case MAIL_TRANSACTION_APPEND: {
		const struct mail_index_record *rec, *end;

		end = CONST_PTR_OFFSET(data, hdr->size);
		for (rec = data; rec < end; rec++) {
			ret = sync_append(rec, ctx);
			if (ret <= 0)
				break;
		}
		break;
	}
	case MAIL_TRANSACTION_EXPUNGE:
	case MAIL_TRANSACTION_EXPUNGE|MAIL_TRANSACTION_EXPUNGE_PROT: {
		const struct mail_transaction_expunge *rec = data, *end;
		ARRAY_TYPE(seq_range) seqs;
		uint32_t seq1, seq2;

		if ((hdr->type & MAIL_TRANSACTION_EXTERNAL) == 0) {
			/* this is simply a request for expunge */
			break;
		}
		t_array_init(&seqs, 64);
		end = CONST_PTR_OFFSET(data, hdr->size);
		for (; rec != end; rec++) {
			if (mail_index_lookup_seq_range(ctx->view,
					rec->uid1, rec->uid2, &seq1, &seq2))
				seq_range_array_add_range(&seqs, seq1, seq2);
		}
		sync_expunge_range(ctx, &seqs);
		break;
	}
	case MAIL_TRANSACTION_EXPUNGE_GUID:
	case MAIL_TRANSACTION_EXPUNGE_GUID|MAIL_TRANSACTION_EXPUNGE_PROT: {
		const struct mail_transaction_expunge_guid *rec = data, *end;
		ARRAY_TYPE(seq_range) seqs;
		uint32_t seq;

		if ((hdr->type & MAIL_TRANSACTION_EXTERNAL) == 0) {
			/* this is simply a request for expunge */
			break;
		}
		t_array_init(&seqs, 64);
		end = CONST_PTR_OFFSET(data, hdr->size);
		for (; rec != end; rec++) {
			i_assert(rec->uid != 0);

			if (mail_index_lookup_seq(ctx->view, rec->uid, &seq))
				seq_range_array_add(&seqs, seq);
		}

		sync_expunge_range(ctx, &seqs);
		break;
	}
	case MAIL_TRANSACTION_FLAG_UPDATE: {
		const struct mail_transaction_flag_update *rec, *end;

		end = CONST_PTR_OFFSET(data, hdr->size);
		for (rec = data; rec < end; rec++) {
			ret = sync_flag_update(rec, ctx);
			if (ret <= 0)
				break;
		}
		break;
	}
	case MAIL_TRANSACTION_HEADER_UPDATE: {
		const struct mail_transaction_header_update *rec;
		unsigned int i;

		for (i = 0; i < hdr->size; ) {
			rec = CONST_PTR_OFFSET(data, i);
			ret = sync_header_update(rec, ctx);
			if (ret <= 0)
				break;

			i += sizeof(*rec) + rec->size;
			if ((i % 4) != 0)
				i += 4 - (i % 4);
		}
		break;
	}
	case MAIL_TRANSACTION_EXT_INTRO: {
		const struct mail_transaction_ext_intro *rec = data;
		unsigned int i;
		uint32_t prev_seq;
		uoff_t prev_offset;

		mail_transaction_log_view_get_prev_pos(ctx->view->log_view,
						       &prev_seq, &prev_offset);
		ctx->ext_intro_seq = prev_seq;
		ctx->ext_intro_offset = prev_offset;
		ctx->ext_intro_end_offset =
			prev_offset + hdr->size + sizeof(*hdr);

		for (i = 0; i < hdr->size; ) {
			if (i + sizeof(*rec) > hdr->size) {
				/* should be just extra padding */
				break;
			}

			rec = CONST_PTR_OFFSET(data, i);
			/* name_size checked by _log_view_next() */
			i_assert(i + sizeof(*rec) + rec->name_size <= hdr->size);

			ret = mail_index_sync_ext_intro(ctx, rec);
			if (ret <= 0)
				break;

			i += sizeof(*rec) + rec->name_size;
			if ((i % 4) != 0)
				i += 4 - (i % 4);
		}
		break;
	}
	case MAIL_TRANSACTION_EXT_RESET: {
		struct mail_transaction_ext_reset rec;

		/* old versions have only new_reset_id */
		if (hdr->size < sizeof(uint32_t)) {
			mail_index_sync_set_corrupted(ctx,
				"ext reset: invalid record size");
			ret = -1;
			break;
		}
		i_zero(&rec);
		memcpy(&rec, data, I_MIN(hdr->size, sizeof(rec)));
		ret = mail_index_sync_ext_reset(ctx, &rec);
		break;
	}
	case MAIL_TRANSACTION_EXT_HDR_UPDATE: {
		const struct mail_transaction_ext_hdr_update *rec;
		unsigned int i;

		for (i = 0; i < hdr->size; ) {
			rec = CONST_PTR_OFFSET(data, i);

			if (i + sizeof(*rec) > hdr->size ||
			    i + sizeof(*rec) + rec->size > hdr->size) {
				mail_index_sync_set_corrupted(ctx,
					"ext hdr update: invalid record size");
				ret = -1;
				break;
			}

			ret = mail_index_sync_ext_hdr_update(ctx, rec->offset,
							     rec->size, rec + 1);
			if (ret <= 0)
				break;

			i += sizeof(*rec) + rec->size;
			if ((i % 4) != 0)
				i += 4 - (i % 4);
		}
		break;
	}
	case MAIL_TRANSACTION_EXT_HDR_UPDATE32: {
		const struct mail_transaction_ext_hdr_update32 *rec;
		unsigned int i;

		for (i = 0; i < hdr->size; ) {
			rec = CONST_PTR_OFFSET(data, i);

			if (i + sizeof(*rec) > hdr->size ||
			    i + sizeof(*rec) + rec->size > hdr->size) {
				mail_index_sync_set_corrupted(ctx,
					"ext hdr update: invalid record size");
				ret = -1;
				break;
			}

			ret = mail_index_sync_ext_hdr_update(ctx, rec->offset,
							     rec->size, rec + 1);
			if (ret <= 0)
				break;

			i += sizeof(*rec) + rec->size;
			if ((i % 4) != 0)
				i += 4 - (i % 4);
		}
		break;
	}
	case MAIL_TRANSACTION_EXT_REC_UPDATE: {
		const struct mail_transaction_ext_rec_update *rec;
		unsigned int i, record_size;

		if (ctx->cur_ext_map_idx == (uint32_t)-1) {
			mail_index_sync_set_corrupted(ctx,
				"Extension record updated "
				"without intro prefix");
			ret = -1;
			break;
		}

		if (ctx->cur_ext_ignore) {
			ret = 1;
			break;
		}

		/* the record is padded to 32bits in the transaction log */
		record_size = (sizeof(*rec) + ctx->cur_ext_record_size + 3) & ~3U;

		for (i = 0; i < hdr->size; i += record_size) {
			rec = CONST_PTR_OFFSET(data, i);

			if (i + record_size > hdr->size) {
				mail_index_sync_set_corrupted(ctx,
					"ext rec update: invalid record size");
				ret = -1;
				break;
			}

			ret = mail_index_sync_ext_rec_update(ctx, rec);
			if (ret <= 0)
				break;
		}
		break;
	}
	case MAIL_TRANSACTION_EXT_ATOMIC_INC: {
		const struct mail_transaction_ext_atomic_inc *rec, *end;

		if (ctx->cur_ext_map_idx == (uint32_t)-1) {
			mail_index_sync_set_corrupted(ctx,
				"Extension record updated "
				"without intro prefix");
			ret = -1;
			break;
		}

		if (ctx->cur_ext_ignore) {
			ret = 1;
			break;
		}

		end = CONST_PTR_OFFSET(data, hdr->size);
		for (rec = data; rec < end; rec++) {
			ret = mail_index_sync_ext_atomic_inc(ctx, rec);
			if (ret <= 0)
				break;
		}
		break;
	}
	case MAIL_TRANSACTION_KEYWORD_UPDATE: {
		const struct mail_transaction_keyword_update *rec = data;

		ret = mail_index_sync_keywords(ctx, hdr, rec);
		break;
	}
	case MAIL_TRANSACTION_KEYWORD_RESET: {
		const struct mail_transaction_keyword_reset *rec = data;

		ret = mail_index_sync_keywords_reset(ctx, hdr, rec);
		break;
	}
	case MAIL_TRANSACTION_MODSEQ_UPDATE: {
		const struct mail_transaction_modseq_update *rec = data;

		ret = sync_modseq_update(ctx, rec, hdr->size);
		break;
	}
	case MAIL_TRANSACTION_INDEX_DELETED:
		if ((hdr->type & MAIL_TRANSACTION_EXTERNAL) == 0) {
			/* next sync finishes the deletion */
			ctx->view->index->index_delete_requested = TRUE;
		} else {
			/* transaction log reading handles this */
		}
		break;
	case MAIL_TRANSACTION_INDEX_UNDELETED:
		ctx->view->index->index_delete_requested = FALSE;
		break;
	case MAIL_TRANSACTION_BOUNDARY:
		break;
	case MAIL_TRANSACTION_ATTRIBUTE_UPDATE:
		break;
	default:
		mail_index_sync_set_corrupted(ctx,
			"Unknown transaction record type 0x%x",
			(hdr->type & MAIL_TRANSACTION_TYPE_MASK));
		ret = -1;
		break;
	}
	return ret;
}

int mail_index_sync_record(struct mail_index_sync_map_ctx *ctx,
			   const struct mail_transaction_header *hdr,
			   const void *data)
{
	int ret;

	T_BEGIN {
		ret = mail_index_sync_record_real(ctx, hdr, data);
	} T_END;
	return ret;
}

void mail_index_sync_map_init(struct mail_index_sync_map_ctx *sync_map_ctx,
			      struct mail_index_view *view,
			      enum mail_index_sync_handler_type type)
{
	i_zero(sync_map_ctx);
	sync_map_ctx->view = view;
	sync_map_ctx->cur_ext_map_idx = (uint32_t)-1;
	sync_map_ctx->type = type;
	sync_map_ctx->modseq_ctx = mail_index_modseq_sync_begin(sync_map_ctx);

	mail_index_sync_init_handlers(sync_map_ctx);
}

void mail_index_sync_map_deinit(struct mail_index_sync_map_ctx *sync_map_ctx)
{
	i_assert(sync_map_ctx->modseq_ctx == NULL);

	buffer_free(&sync_map_ctx->unknown_extensions);
	if (sync_map_ctx->expunge_handlers_used)
		mail_index_sync_deinit_expunge_handlers(sync_map_ctx);
	mail_index_sync_deinit_handlers(sync_map_ctx);
}

static void mail_index_sync_update_hdr_dirty_flag(struct mail_index_map *map)
{
	const struct mail_index_record *rec;
	uint32_t seq;

	if ((map->hdr.flags & MAIL_INDEX_HDR_FLAG_HAVE_DIRTY) != 0 ||
	    (map->index->flags & MAIL_INDEX_OPEN_FLAG_NO_DIRTY) != 0)
		return;

	/* do we have dirty flags anymore? */
	for (seq = 1; seq <= map->rec_map->records_count; seq++) {
		rec = MAIL_INDEX_REC_AT_SEQ(map, seq);
		if ((rec->flags & MAIL_INDEX_MAIL_FLAG_DIRTY) != 0) {
			map->hdr.flags |= MAIL_INDEX_HDR_FLAG_HAVE_DIRTY;
			break;
		}
	}
}

#ifdef DEBUG
void mail_index_map_check(struct mail_index_map *map)
{
	const struct mail_index_header *hdr = &map->hdr;
	unsigned int del = 0, seen = 0;
	uint32_t seq, prev_uid = 0;

	if (getenv("DEBUG_IGNORE_INDEX_CORRUPTION") != NULL)
		return;

	i_assert(hdr->messages_count <= map->rec_map->records_count);
	for (seq = 1; seq <= hdr->messages_count; seq++) {
		const struct mail_index_record *rec;

		rec = MAIL_INDEX_REC_AT_SEQ(map, seq);
		i_assert(rec->uid > prev_uid);
		prev_uid = rec->uid;

		if ((rec->flags & MAIL_DELETED) != 0) {
			i_assert(rec->uid >= hdr->first_deleted_uid_lowwater);
			del++;
		}
		if ((rec->flags & MAIL_SEEN) != 0)
			seen++;
		else
			i_assert(rec->uid >= hdr->first_unseen_uid_lowwater);
	}
	i_assert(del == hdr->deleted_messages_count);
	i_assert(seen == hdr->seen_messages_count);
}
#endif

bool mail_index_sync_map_want_index_reopen(struct mail_index_map *map,
					   enum mail_index_sync_handler_type type)
{
	struct mail_index *index = map->index;

	if (index->log->head == NULL)
		return TRUE;

	uoff_t start_offset = type == MAIL_INDEX_SYNC_HANDLER_FILE ?
		map->hdr.log_file_tail_offset : map->hdr.log_file_head_offset;
	/* don't check this if mmap is disabled, because reopening
	   index causes sync to get lost. */
	if ((index->flags & MAIL_INDEX_OPEN_FLAG_MMAP_DISABLE) == 0) {
		uoff_t log_size, index_size;

		if (index->fd == -1 &&
		    index->log->head->hdr.prev_file_seq != 0) {
			/* we don't know the index's size, so use the
			   smallest index size we're willing to read */
			index_size = MAIL_INDEX_SYNC_MIN_READ_INDEX_SIZE;
		} else {
			index_size = map->hdr.header_size +
				map->rec_map->records_count *
				map->hdr.record_size;
		}

		/* this isn't necessary correct currently, but it should be
		   close enough */
		log_size = index->log->head->last_size;
		if (log_size > start_offset &&
		    log_size - start_offset > index_size)
			return TRUE;
	}
	return FALSE;
}

int mail_index_sync_map(struct mail_index_map **_map,
			enum mail_index_sync_handler_type type,
			const char **reason_r)
{
	struct mail_index_map *map = *_map;
	struct mail_index *index = map->index;
	struct mail_index_view *view;
	struct mail_index_sync_map_ctx sync_map_ctx;
	const struct mail_transaction_header *thdr;
	const void *tdata;
	uint32_t prev_seq;
	uoff_t start_offset, prev_offset;
	const char *reason, *error;
	int ret;
	bool had_dirty, reset;

	i_assert(index->log->head != NULL);
	i_assert(index->map == map || type == MAIL_INDEX_SYNC_HANDLER_VIEW);

	start_offset = type == MAIL_INDEX_SYNC_HANDLER_FILE ?
		map->hdr.log_file_tail_offset : map->hdr.log_file_head_offset;

	view = mail_index_view_open_with_map(index, map);
	ret = mail_transaction_log_view_set(view->log_view,
					    map->hdr.log_file_seq, start_offset,
					    (uint32_t)-1, UOFF_T_MAX,
					    &reset, &reason);
	if (ret <= 0) {
		mail_index_view_close(&view);
		if (ret < 0) {
			/* I/O failure */
			return -1;
		}
		/* the seq/offset is probably broken */
		*reason_r = t_strdup_printf(
			"Lost log for seq=%u offset=%"PRIuUOFF_T": %s "
			"(initial_mapped=%d)",
			map->hdr.log_file_seq, start_offset, reason,
			index->initial_mapped ? 1 : 0);
		return 0;
	}

	mail_transaction_log_get_head(index->log, &prev_seq, &prev_offset);
	if (prev_seq != map->hdr.log_file_seq ||
	    prev_offset - map->hdr.log_file_tail_offset >
	    		index->optimization_set.index.rewrite_min_log_bytes) {
		/* we're reading more from log than we would have preferred.
		   remember that we probably want to rewrite index soon. */
		index->index_min_write = TRUE;
	}

	/* view referenced the map. avoid unnecessary map cloning by
	   unreferencing the map while view exists. */
	map->refcount--;

	had_dirty = (map->hdr.flags & MAIL_INDEX_HDR_FLAG_HAVE_DIRTY) != 0;
	if (had_dirty)
		map->hdr.flags &= ENUM_NEGATE(MAIL_INDEX_HDR_FLAG_HAVE_DIRTY);

	mail_transaction_log_view_get_prev_pos(view->log_view,
					       &prev_seq, &prev_offset);

	mail_index_sync_map_init(&sync_map_ctx, view, type);
	if (reset) {
		/* Reset the entire index. Leave only indexid and
		   log_file_seq. */
		mail_transaction_log_view_get_prev_pos(view->log_view,
						       &prev_seq, &prev_offset);
		map = mail_index_map_alloc(index);
		if ((index->map->hdr.flags & MAIL_INDEX_HDR_FLAG_FSCKD) != 0)
			map->hdr.flags |= MAIL_INDEX_HDR_FLAG_FSCKD;
		map->hdr.log_file_seq = prev_seq;
		map->hdr.log_file_tail_offset = 0;
		mail_index_sync_replace_map(&sync_map_ctx, map);
	}
	map = NULL;

	/* FIXME: when transaction sync lock is removed, we'll need to handle
	   the case when a transaction is committed while mailbox is being
	   synced ([synced transactions][new transaction][ext transaction]).
	   this means int_offset contains [synced] and ext_offset contains
	   all */
	while ((ret = mail_transaction_log_view_next(view->log_view, &thdr,
						     &tdata)) > 0) {
		mail_transaction_log_view_get_prev_pos(view->log_view,
						       &prev_seq, &prev_offset);

		if (LOG_IS_BEFORE(prev_seq, prev_offset,
				  view->map->hdr.log_file_seq,
				  view->map->hdr.log_file_head_offset)) {
			/* this has been synced already. */
			i_assert(type == MAIL_INDEX_SYNC_HANDLER_FILE);
			continue;
		}

		/* we'll just skip over broken entries */
		(void)mail_index_sync_record(&sync_map_ctx, thdr, tdata);
	}
	map = view->map;

	if (had_dirty)
		mail_index_sync_update_hdr_dirty_flag(map);
	mail_index_modseq_sync_end(&sync_map_ctx.modseq_ctx);

	mail_index_sync_update_log_offset(&sync_map_ctx, view->map, TRUE);

#ifdef DEBUG
	mail_index_map_check(map);
#endif
	i_assert(map->hdr.indexid == index->indexid || map->hdr.indexid == 0);

	/* transaction log tracks internally the current tail offset.
	   besides using header updates, it also updates the offset to skip
	   over following external transactions to avoid extra unneeded log
	   reading. */
	i_assert(map->hdr.log_file_seq == index->log->head->hdr.file_seq);
	if (map->hdr.log_file_tail_offset < index->log->head->max_tail_offset) {
		map->hdr.log_file_tail_offset =
			index->log->head->max_tail_offset;
	}

	buffer_write(map->hdr_copy_buf, 0, &map->hdr, sizeof(map->hdr));
	if (!MAIL_INDEX_MAP_IS_IN_MEMORY(map)) {
		memcpy(map->rec_map->mmap_base, map->hdr_copy_buf->data,
		       map->hdr_copy_buf->used);
	}

	/* restore refcount before closing the view. this is necessary also
	   if map got cloned, because view closing would otherwise destroy it */
	map->refcount++;
	mail_index_sync_map_deinit(&sync_map_ctx);
	mail_index_view_close(&view);

	i_assert(index->map == map || type == MAIL_INDEX_SYNC_HANDLER_VIEW);

	if (mail_index_map_check_header(map, &error) <= 0) {
		mail_index_set_error(index,
			"Synchronization corrupted index header %s: %s",
			index->filepath, error);
		(void)mail_index_fsck(index);
		map = index->map;
	} else if (sync_map_ctx.errors) {
		/* make sure the index looks valid now */
		(void)mail_index_fsck(index);
		map = index->map;
	}

	*_map = map;
	return ret < 0 ? -1 : 1;
}
