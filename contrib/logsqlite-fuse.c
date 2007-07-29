
/* logsqlite-fuse
 * (C) 2007 Michał Górny
 * covered under GPL2
 *
 * compile like:
 * 	gcc ${CFLAGS} $(pkg-config --cflags fuse sqlite3) -o logsqlite-fuse logsqlite-fuse.c $(pkg-config --libs fuse sqlite3) 
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <time.h>

#include <errno.h>

#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <sqlite3.h>

#define QUERY_COUNT 7
#define READ_ROW_COUNT "20"
#define BUF_SIZE_FACTOR 4096

#ifdef NODEBUG
#define DEBUG(x...)
#else

#ifndef DEBUG
#define DEBUG(x...) fprintf(stderr, x)
#endif

#ifndef FUSE_DEBUG
#define FUSE_DEBUG ",debug"
#endif

#endif

typedef struct {
	char		*sid,
			*uid,
			*data;
	off_t		data_off;
	size_t		data_size,
			buf_size;
	int		db_off,
			eof;
	time_t		last_use;
	void		*next;
} myBuffer_t;

typedef struct {
	sqlite3		*db;
	sqlite3_stmt	*stmt[QUERY_COUNT];
	myBuffer_t	*buffers;
} myDB_t;

static const char *queries[QUERY_COUNT+1] = {
	"SELECT ts FROM log_msg ORDER BY ts DESC LIMIT 1;",
	"SELECT ts FROM log_msg WHERE session = ?1 ORDER BY ts DESC LIMIT 1;",
	"SELECT ts FROM log_msg WHERE session = ?1 AND uid = ?2 ORDER BY ts DESC LIMIT 1;",
	"SELECT DISTINCT session FROM log_msg ORDER BY session ASC LIMIT -1 OFFSET ?1;",
	"SELECT DISTINCT uid FROM log_msg WHERE session = ?2 ORDER BY uid ASC LIMIT -1 OFFSET ?1;",
	"SELECT type, sent, uid, nick, ts, sentts, body FROM log_msg WHERE session = ?1 AND uid = ?2 ORDER BY ts ASC LIMIT " READ_ROW_COUNT " OFFSET ?3;",
	"DELETE FROM log_msg WHERE session = ?1 AND uid = ?2;",
	NULL
};

enum statement_n {
	GET_NEWEST,
	GET_NEWEST_SESSION,
	GET_NEWEST_UID,
	GET_SESSIONS,
	GET_UIDS,
	GET_DATA,
	REMOVE_UID
};

	/* garbage cleaner for buffers */
void myGC(myDB_t *db) {
	const time_t now = time(NULL);
	myBuffer_t *curr, *prev;

	for (curr = db->buffers, prev = NULL; curr; prev = curr, curr = curr->next) {
		while (curr->last_use - now > BUF_MAX_UNUSED) {
			if (buf->data)
				free(buf->data);
			if (prev)
				prev->next	= buf->next;
			else
				db->buffers	= buf->next;

			curr = buf->next;
			free(buf);
		}
	}
}

int mySplitPath(const char *path, const char **sid, const char **uid) {
	static char sidbuf[PATH_MAX], uidbuf[PATH_MAX];
	const char *tmp;

	if (strlen(path) > PATH_MAX) return -ENAMETOOLONG;

	*sid = sidbuf;
	*uid = uidbuf;

		/* XXX: rewrite, replace with some magic loop */

	path++; /* skip leading '/' */
	if ((tmp = strchr(path, '/'))) {
		strncpy(sidbuf, path, tmp-path);
		sidbuf[tmp-path] = 0;
		path = tmp+1;

		if ((tmp = strchr(path, '/'))) {
			strncpy(uidbuf, path, tmp-path);
			uidbuf[tmp-path] = 0;
			path = tmp+1;
			return 2 + (*path != 0);
		} else {
			strcpy(uidbuf, path);
			return 1 + (*path != 0);
		}
	} else {
		strcpy(sidbuf, path);
		return 0 + (*path != 0);
	}
}

int myGetAttr(const char *path, struct stat *out) {
	const char *sid, *uid;
	const int pathargs = mySplitPath(path, &sid, &uid);
	myDB_t *db = fuse_get_context()->private_data;
	sqlite3_stmt *stmt;
	int timestamp;

	switch (pathargs) {
		case 0:
			stmt = db->stmt[GET_NEWEST];
			break;
		case 1:
			stmt = db->stmt[GET_NEWEST_SESSION];
			sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_STATIC);
			break;
		default:
			stmt = db->stmt[GET_NEWEST_UID];
			sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_STATIC);
			sqlite3_bind_text(stmt, 2, uid, -1, SQLITE_STATIC);
	}

	if (sqlite3_step(stmt) != SQLITE_ROW) {
		sqlite3_reset(stmt);
		return -ENOENT;
	}

	timestamp = sqlite3_column_int(stmt, 0);
	sqlite3_reset(stmt);
	if (pathargs > 2)
		return -ENOTDIR;

	if (pathargs < 2) {
		out->st_mode	= S_IFDIR | 0555;
		out->st_nlink	= 3; /* XXX: + subdirectory count */
	} else {
		out->st_mode	= S_IFREG | 0444;
		out->st_nlink	= 1;
	}
	out->st_uid	= getuid();
	out->st_gid	= getgid();
	out->st_atime	= out->st_mtime	= out->st_ctime	= timestamp;

	return 0;
}

int myReadDir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
	const char *sid, *uid;
	const int pathargs = mySplitPath(path, &sid, &uid);
	myDB_t *db = fuse_get_context()->private_data;
	sqlite3_stmt *stmt;
	char *nextrow = NULL, *myrow = NULL;
	
	if (pathargs == 0)
		stmt = db->stmt[GET_SESSIONS];
	else {
		stmt = db->stmt[GET_UIDS];
		sqlite3_bind_text(stmt, 2, sid, -1, SQLITE_STATIC);
	}
	sqlite3_bind_int(stmt, 1, offset);

	DEBUG("myReadDir: path = %s, off = %d\n", path, offset);

	do {
		if (myrow)
			free(myrow);
		myrow = nextrow;
		nextrow = (sqlite3_step(stmt) == SQLITE_ROW ? strdup(sqlite3_column_text(stmt, 0)) : NULL);

		DEBUG("myReadDir-loop: myrow = %s, nextrow = %s, offset = %d\n", myrow, nextrow, offset);

		if (myrow && filler(buf, myrow, NULL, (nextrow ? ++offset : 0)) == 1)
				break;
	} while (nextrow);
	sqlite3_reset(stmt);

	if (myrow)
		free(myrow);

	return 0;
}

	/* offset = -1 -> remove */
myBuffer_t *myBufferFind(myDB_t *db, const char *sid, const char *uid, off_t offset) {
	myBuffer_t *out, *prev;

	for (out = db->buffers, prev = NULL; out && (strcmp(out->sid, sid) || strcmp(out->uid, uid)); prev = out, out = out->next);

	if (!out) { /* create */
		if (offset == -1)
			return NULL;

		out		= malloc(sizeof(myBuffer_t));
		out->sid	= strdup(sid);
		out->uid	= strdup(uid);
		out->data	= NULL;
		out->data_off	= 0;
		out->data_size	= 0;
		out->buf_size 	= 0;
		out->db_off	= 0;
		out->eof	= 0;
		out->next	= 0;
		if (prev)
			prev->next	= out;
		else
			db->buffers	= out;
	} else if (offset == -1) { /* remove */
		if (out->data)
			free(out->data);
		if (prev)
			prev->next	= out->next;
		else
			db->buffers	= out->next;
		free(out);

		return NULL;
	} else if (offset < out->data_off) { /* rewind */
		if (out->data) {
			free(out->data);
			out->data	= NULL;
		}
		out->data_off	= 0;
		out->data_size	= 0;
		out->buf_size 	= 0;
		out->db_off	= 0;
		out->eof	= 0;
	}
	out->last_use	= time(NULL);

	return out;
}

/* based on log_escape() from EKG2 */
	/* XXX: add/fix utf-8 handling ? */
const char *myBodyEscape(const char *in) {
	static char *out	= NULL;
	static size_t bufsize	= 0;

	const int needbuf	= strlen(in) * 2 + 3;

	if (bufsize < needbuf) {
		bufsize = (needbuf / BUF_SIZE_FACTOR + 1) * BUF_SIZE_FACTOR;
			/* we don't use realloc(), 'cause we don't need data copying */
		if (out) free(out);
		out = malloc(bufsize);
		out[0] = '"';
	}

	char *p, *q;
	int hadto = 0;

	for (p = in, q = out+1; *p; p++, q++) {
		switch (*p) {
			case '\n':
				*q++	= '\\';
				*q	= 'n';
				hadto	= 1;
				break;
			case '\r':
				*q++	= '\\';
				*q	= 'r';
				hadto	= 1;
				break;
			case '\\':
			case '"':
			case '\'':
				*q++	= '\\';
				hadto	= 1;
			default:
				*q	= *p;
		}
	}

	if (hadto)
		*q++	= '"';

	*q	= 0;
	
	return (hadto ? out : out+1);
}

void myBufferStep(sqlite3_stmt *stmt, myBuffer_t *buf) {
	if (buf->eof)
		return;

	buf->data_off	+= buf->data_size;
	buf->data_size	= 0;
	if (!buf->data) {
		buf->data	= malloc(BUF_SIZE_FACTOR);
		buf->buf_size	= BUF_SIZE_FACTOR;
	}

	DEBUG("myBufferStep: data_off = %d, buf_size = %d\n", buf->data_off, buf->buf_size);

	sqlite3_bind_int(stmt, 3, buf->db_off);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const int is_sent	= sqlite3_column_int(stmt, 1);
		const char *type	= sqlite3_column_text(stmt, 0);
		char *body		= myBodyEscape(sqlite3_column_text(stmt, 6));
		char tsbuf[12];
		int r;

		if (!strcmp(type, "msg")) {
			if (is_sent)	type = "msgsend";
			else		type = "msgrecv";
		} else if (!strcmp(type, "system"))
			type = "msgsystem";
		else { /* "chat" */
			if (is_sent)	type = "chatsend";
			else		type = "chatrecv";
		}

		if (is_sent)
			tsbuf[0]	= 0;
		else {
			const int sts	= sqlite3_column_int(stmt, 5);
			r	= snprintf(tsbuf, sizeof(tsbuf), "%d,", (sts ? sts : sqlite3_column_int(stmt, 4)));
			if (r == -1 || r >= sizeof(tsbuf))
				tsbuf[0]	= 0; /* ignore second timestamp, if shit happens */
		}

		do {
			r	= snprintf(buf->data + buf->data_size, buf->buf_size - buf->data_size, "%s,%s,%s,%d,%s%s\n",
					type, sqlite3_column_text(stmt, 2), sqlite3_column_text(stmt, 3),
					sqlite3_column_int(stmt, 4), tsbuf, body);

			if (r >= (buf->buf_size - buf->data_size)) {
				buf->buf_size	= ((buf->data_size + r + 1) / BUF_SIZE_FACTOR + 1) * BUF_SIZE_FACTOR;
				buf->data	= realloc(buf->data, buf->buf_size);
				r		= -1;
			} else if (r == -1) {
				buf->buf_size	= buf->data_size + BUF_SIZE_FACTOR;
				buf->data	= realloc(buf->data, buf->buf_size);
			} else
				buf->data_size += r;
		} while (r == -1);

		buf->db_off++;
	}
	sqlite3_reset(stmt);

	if (buf->data_size == 0) /* no data read */
		buf->eof	= 1;
}

int myReadFile(const char *path, char *out, size_t count, off_t offset, struct fuse_file_info *fi) {
	const char *sid, *uid;
	myDB_t *db = fuse_get_context()->private_data;
	sqlite3_stmt *stmt = db->stmt[GET_DATA];
	myBuffer_t *buf;
	int written = 0;

	mySplitPath(path, &sid, &uid);
	buf = myBufferFind(db, sid, uid, offset);
	sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, uid, -1, SQLITE_STATIC);

	DEBUG("myReadFile: path = %s, count = %d, offset = %d\n", path, count, offset);
	while (count > 0) {
		while (offset >= buf->data_off + buf->data_size && !buf->eof)
			myBufferStep(stmt, buf);
		if (buf->eof)
			break;

		const int toend		= buf->data_size - buf->data_off;
		const int towrite	= (count > toend ? toend : count);

		DEBUG("myReadFile-loop: count = %d, offset = %d, toend = %d, towrite = %d, written = %d\n", count, offset, toend, towrite, written);
		memcpy(out, buf->data + (offset - buf->data_off), towrite);
		
		out	+= towrite;
		count	-= towrite;
		offset	+= towrite;
		written	+= towrite;
	}

	return written;
};

int myUnlink(const char *path) {
	const char *sid, *uid;
	myDB_t *db = fuse_get_context()->private_data;
	sqlite3_stmt *stmt = db->stmt[REMOVE_UID];

	mySplitPath(path, &sid, &uid);
	sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, uid, -1, SQLITE_STATIC);

	sqlite3_reset(stmt);
	if (sqlite3_step(stmt) != SQLITE_DONE)
		return -EIO;

	return 0;
}

int myReleaseFile(const char *path, struct fuse_file_info *fi) {
	const char *sid, *uid;
	myDB_t *db = fuse_get_context()->private_data;

	mySplitPath(path, &sid, &uid);
	myBufferFind(db, sid, uid, -1);

	return 0;
}

static struct fuse_operations ops = {
	.getattr	= &myGetAttr,
	.readdir	= &myReadDir,
	.read		= &myReadFile,
	.unlink		= &myUnlink,
	.release	= &myReleaseFile
};

int main(int argc, char *argv[]) {
	myDB_t	db;

	if (argc < 3) {
		DEBUG("SYNOPSIS: %s database-path mount-point\n", argv[0]);

		return 1;
	}

	sqlite3_open(argv[1], &(db.db));
	{		/* make the db really open */
		char *err;
		if (sqlite3_exec(db.db, "SELECT * FROM log_msg LIMIT 1;", NULL, NULL, &err) != SQLITE_OK) {
			DEBUG("Unable to open the database: %s.\n", err);
			sqlite3_free(err);
			return 2;
		}
	}

	{
		const char **query;
		sqlite3_stmt **stmt;

		for (query = queries, stmt = db.stmt; *query; query++, stmt++) {
			if (sqlite3_prepare(db.db, *query, -1, stmt, NULL) != SQLITE_OK) {
				DEBUG("Unable to compile queries: %s [on %s].\n", sqlite3_errmsg(db.db), *query);
				return 3;
			}
		}
	}
	db.buffers = NULL;

	{
		int fuse_argc = 4;
		char *fuse_argv[] = {"fusermount", "-o", "direct_io,rw" FUSE_DEBUG, argv[2], NULL};

		fuse_main(fuse_argc, fuse_argv, &ops, &db);
	}

	return 0;
}
