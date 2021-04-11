/*
 * Copyright (c) 2020 trinity-tech
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifdef HAVE_TIME_H
#include <time.h>
#endif
#include <assert.h>

#include <crystal.h>
#include <sqlite3.h>

#include "did.h"
#include "ver.h"
#include "db.h"

#define TAG_DB "[Feedsd.Db  ]: "

typedef struct {
    UserInfo info;
    sqlite3_stmt *stmt;
} DBUserInfo;

typedef void *(*Row2Raw)(sqlite3_stmt *);
struct DBObjIt {
    sqlite3_stmt *stmt;
    Row2Raw cb;
};

static sqlite3 *db;

static
int check_table_valid(const char *table_name, int item_num)
{
    sqlite3_stmt *stmt;
    char sql[128] = {0};
    int rc;

    snprintf(sql, sizeof(sql), 
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='%s'", 
        table_name);
    vlogD(TAG_DB "Ready to check table: %s", table_name);
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "Check table valid sqlite3_prepare_v2() failed");
        return -1;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        vlogE(TAG_DB "Check table valid DB failed");
        sqlite3_finalize(stmt);
        return -1;
    }
    rc = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    stmt = NULL;

    if (1 == rc) {
        memset(sql, 0, sizeof(sql));
        snprintf(sql, sizeof(sql), "PRAGMA table_info('%s')", table_name);
        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
        if (rc) {
            vlogE(TAG_DB "PRAGMA table_info sqlite3_prepare_v2() failed");
            return -1;
        }
        while(sqlite3_step(stmt) == SQLITE_ROW) {    //collect the column num of table
            rc = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
        if (rc + 1 == item_num) {    //check column match the expection
            vlogD(TAG_DB "Table %s is newest version", table_name);
            return 2;
        }

        return 1;
    } else if(0 == rc) {
        return 0;
    } else {
        return -1;
    }
}

static
int backup_table(const char *table_name)
{
    sqlite3_stmt *stmt;
    char sql[128] = {0};
    int rc;

    snprintf(sql, sizeof(sql), "ALTER TABLE %s RENAME TO %s_backup",
            table_name, table_name);
    vlogD(TAG_DB "Ready to backup table: from %s to %s_backup", table_name, table_name);
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        vlogE(TAG_DB "Backup %s table failed", table_name);
        return -1;
    }

    return 0;
}

static
int delete_old_index(const char *table_name)
{
    sqlite3_stmt *stmt;
    char sql[128] = {0};
    int rc;

    snprintf(sql, sizeof(sql), "DROP INDEX %s_created_at_index", table_name);
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "Delete %s_created_at_index sqlite3_prepare_v2() failed",
            table_name);
        return -1;
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        vlogE(TAG_DB "Delete %s_created_at_index failed", table_name);
        return -1;
    }

    memset(sql, 0, sizeof(sql));
    snprintf(sql, sizeof(sql), "DROP INDEX %s_updated_at_index", table_name);
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "Delete %s_updated_at_index sqlite3_prepare_v2() failed",
            table_name);
        return -1;
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        vlogE(TAG_DB "Delete %s_updated_at_index failed", table_name);
        return -1;
    }

    vlogD(TAG_DB "Delete table %s old index successfully", table_name);
    return 0;
}

static
int create_new_index(const char *table_name, const char *para)
{
    sqlite3_stmt *stmt;
    char sql[128] = {0};
    int rc;

    snprintf(sql, sizeof(sql), 
        "CREATE INDEX %s_created_at_index ON %s (%screated_at)", 
        table_name, table_name, para);
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "Creating %s_created_at_index sqlite3_prepare_v2() failed", 
            table_name);
        return -1;
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        vlogE(TAG_DB "Creating %s_created_at_index failed", table_name);
        return -1;
    }

    memset(sql, 0, sizeof(sql));
    snprintf(sql, sizeof(sql), 
        "CREATE INDEX %s_updated_at_index ON %s (%supdated_at)", 
        table_name, table_name, para);
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "Creating %s_updated_at_index sqlite3_prepare_v2() failed",
            table_name);
        return -1;
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        vlogE(TAG_DB "Creating %s_updated_at_index failed", table_name);
        return -1;
    }

    return 0;
}

static
int create_table_channels(void)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "CREATE TABLE IF NOT EXISTS channels ("
          "  channel_id    INTEGER PRIMARY KEY AUTOINCREMENT,"
          "  created_at    REAL    NOT NULL,"
          "  updated_at    REAL    NOT NULL,"
          "  name          TEXT    NOT NULL UNIQUE,"
          "  intro         TEXT    NOT NULL,"
          "  next_post_id  INTEGER NOT NULL DEFAULT 1,"
          "  subscribers   INTEGER NOT NULL DEFAULT 0,"
          "  status        INTEGER NOT NULL DEFAULT 1,"
          "  iid           TEXT    NOT NULL,"
          "  tip_methods   TEXT    NOT NULL,"
          "  proof         TEXT    NOT NULL,"
          "  memo          TEXT    NOT NULL,"
          "  avatar        BLOB    NOT NULL"
          ")";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "Create channels sqlite3_prepare_v2() failed");
        return -1;
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        vlogE(TAG_DB "Creating channels table failed");
        return -1;
    }

    if (-1 == create_new_index("channels", "")) {
        return -1;
    }

    vlogD(TAG_DB "Create table channels successfully");
    return 0;
}

static
int retrive_channels_data(void)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "INSERT INTO channels SELECT"
          " channel_id, created_at, updated_at, name, intro, next_post_id,"
          " subscribers, 1, '', '', '', '', avatar"
          " FROM channels_backup";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "Retriving channels data sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        vlogE(TAG_DB "Retriving channels data failed");
        return -1;
    }

    vlogD(TAG_DB "Retrive channels data successfully");
    return 0;
}

static
int create_table_posts(void)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "CREATE TABLE IF NOT EXISTS posts ("
          "  channel_id      INTEGER NOT NULL REFERENCES channels(channel_id),"
          "  post_id         INTEGER NOT NULL,"
          "  created_at      REAL    NOT NULL,"
          "  updated_at      REAL    NOT NULL,"
          "  next_comment_id INTEGER NOT NULL DEFAULT 1,"
          "  likes           INTEGER NOT NULL DEFAULT 0,"
          "  status          INTEGER NOT NULL DEFAULT 0,"
          "  iid             TEXT    NOT NULL,"
          "  hash_id         TEXT    NOT NULL,"
          "  proof           TEXT    NOT NULL,"
          "  origin_post_url TEXT    NOT NULL,"
          "  memo            TEXT    NOT NULL,"
          "  thumbnails      BLOB    NOT NULL,"
          "  content         BLOB    NOT NULL,"
          "  PRIMARY KEY(channel_id, post_id)"
          ")";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "Create posts sqlite3_prepare_v2() failed");
        return -1;
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        vlogE(TAG_DB "Creating posts table failed");
        return -1;
    }

    if (-1 == create_new_index("posts", "channel_id, ")) {
        return -1;
    }

    vlogD(TAG_DB "Create table posts successfully");
    return 0;
}

static
int retrive_posts_data(void)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "INSERT INTO posts SELECT"
          " channel_id, post_id, created_at, updated_at, next_comment_id,"
          " likes, status, '', '', '', '', '', '', content"
          " FROM posts_backup";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "Retriving posts data sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        vlogE(TAG_DB "Retriving posts data failed");
        return -1;
    }

    vlogD(TAG_DB "Retrive posts data successfully");
    return 0;
}

static
int create_table_comments(void)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "CREATE TABLE IF NOT EXISTS comments ("
          "  channel_id    INTEGER NOT NULL,"
          "  post_id       INTEGER NOT NULL,"
          "  comment_id    INTEGER NOT NULL,"
          "  refcomment_id INTEGER NOT NULL,"
          "  user_id       INTEGER NOT NULL REFERENCES users(user_id),"
          "  created_at    REAL    NOT NULL,"
          "  updated_at    REAL    NOT NULL,"
          "  likes         INTEGER NOT NULL DEFAULT 0,"
          "  status        INTEGER NOT NULL DEFAULT 0,"
          "  iid           TEXT    NOT NULL,"
          "  hash_id       TEXT    NOT NULL,"
          "  proof         TEXT    NOT NULL,"
          "  memo          TEXT    NOT NULL,"
          "  thumbnails    BLOB    NOT NULL,"
          "  content       BLOB    NOT NULL,"
          "  PRIMARY KEY(channel_id, post_id, comment_id)"
          "  FOREIGN KEY(channel_id, post_id) REFERENCES posts(channel_id, post_id)"
          ")";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "Create comments sqlite3_prepare_v2() failed");
        return -1;
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        vlogE(TAG_DB "Creating comments table failed");
        return -1;
    }

    if (-1 == create_new_index("comments", "channel_id, post_id, ")) {
        return -1;
    }

    vlogD(TAG_DB "Create table comments successfully");
    return 0;
}

static
int retrive_comments_data(void)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "INSERT INTO comments SELECT"
          " channel_id, post_id, comment_id, refcomment_id, user_id,"
          " created_at, updated_at, likes, status, '', '', '', '', '', content"
          " FROM comments_backup";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "Retriving comments data sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        vlogE(TAG_DB "Retriving comments data failed");
        return -1;
    }

    vlogD(TAG_DB "Retrive comments data successfully");
    return 0;
}

static
int create_table_users(void)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "CREATE TABLE IF NOT EXISTS users ("
          "  user_id      INTEGER PRIMARY KEY AUTOINCREMENT,"
          "  did          TEXT NOT NULL UNIQUE,"
          "  name         TEXT NOT NULL DEFAULT 'NA',"
          "  email        TEXT NOT NULL DEFAULT 'NA',"
          "  display_name TEXT NOT NULL DEFAULT 'NA',"
          "  update_at    REAL NOT NULL,"
          "  memo         TEXT NOT NULL,"
          "  avatar       BLOB NOT NULL"
          ")";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "Create users sqlite3_prepare_v2() failed");
        return -1;
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        vlogE(TAG_DB "Creating users table failed");
        return -1;
    }

    vlogD(TAG_DB "Create table users successfully");
    return 0;
}

static
int retrive_users_data(void)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "INSERT INTO users SELECT"
          " user_id, did, name, email, '', '', '', ''"
          " FROM users_backup";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "Retriving users data sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        vlogE(TAG_DB "Retriving users data failed");
        return -1;
    }

    vlogD(TAG_DB "Retrive users data successfully");
    return 0;
}

static
int create_table_subscriptions(void)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "CREATE TABLE IF NOT EXISTS subscriptions ("
          "  user_id    INTEGER NOT NULL REFERENCES users(user_id),"
          "  channel_id INTEGER NOT NULL REFERENCES channels(channel_id),"
          "  create_at  REAL    NOT NULL,"
          "  proof      TEXT    NOT NULL,"
          "  memo       TEXT    NOT NULL,"
          "  PRIMARY KEY(user_id, channel_id)"
          ")";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "Create subscriptions sqlite3_prepare_v2() failed");
        return -1;
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        vlogE(TAG_DB "Creating subscriptions table failed");
        return -1;
    }

    vlogD(TAG_DB "Create table subscriptions successfully");
    return 0;
}

static
int retrive_subscriptions_data(void)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "INSERT INTO subscriptions SELECT"
          " user_id, channel_id, '', '', ''"
          " FROM subscriptions_backup";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "Retriving subscriptions data sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        vlogE(TAG_DB "Retriving subscriptions data failed");
        return -1;
    }

    vlogD(TAG_DB "Retrive subscriptions data successfully");
    return 0;
}

static
int create_table_likes(void)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "CREATE TABLE IF NOT EXISTS likes ("
          "  channel_id INTEGER NOT NULL,"
          "  post_id    INTEGER NOT NULL,"
          "  comment_id INTEGER NOT NULL,"
          "  user_id    INTEGER NOT NULL REFERENCES users(user_id),"
          "  created_at REAL    NOT NULL,"
          "  proof      TEXT    NOT NULL,"
          "  memo       TEXT    NOT NULL,"
          "  FOREIGN KEY(channel_id, post_id) REFERENCES posts(channel_id, post_id)"
          "  PRIMARY KEY(user_id, channel_id, post_id, comment_id)"
          ")";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "Create likes sqlite3_prepare_v2() failed");
        return -1;
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        vlogE(TAG_DB "Creating likes table failed");
        return -1;
    }

    vlogD(TAG_DB "Create table likes successfully");
    return 0;
}

static
int retrive_likes_data(void)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "INSERT INTO likes SELECT"
          " channel_id, post_id, comment_id, user_id, created_at, '', ''"
          " FROM likes_backup";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "Retriving likes data sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        vlogE(TAG_DB "Retriving likes data failed");
        return -1;
    }

    vlogD(TAG_DB "Retrive likes data successfully");
    return 0;
}

static
int create_table_reported_comments(void)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "CREATE TABLE IF NOT EXISTS reported_comments ("
          "  channel_id  INTEGER NOT NULL,"
          "  post_id     INTEGER NOT NULL,"
          "  comment_id  INTEGER NOT NULL,"
          "  reporter_id INTEGER NOT NULL REFERENCES users(user_id),"
          "  created_at  REAL    NOT NULL,"
          "  reasons     TEXT    NOT NULL DEFAULT 'NA',"
          "  PRIMARY KEY(channel_id, post_id, comment_id, reporter_id)"
          "  FOREIGN KEY(channel_id, post_id) REFERENCES posts(channel_id, post_id)"
          ")";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "Create reported_comments sqlite3_prepare_v2() failed");
        return -1;
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        vlogE(TAG_DB "Creating reported_comments table failed");
        return -1;
    }

    vlogD(TAG_DB "Create table reported_comments successfully");
    return 0;
}

static
int create_table_tipping(void)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "CREATE TABLE IF NOT EXISTS tipping ("
          "  id           INTEGER NOT NULL,"
          "  channel_id   INTEGER NOT NULL,"
          "  post_id      INTEGER NOT NULL,"
          "  created_at   REAL    NOT NULL,"
          "  token_mount  REAL    NOT NULL,"
          "  token_symbol TEXT    NOT NULL,"
          "  proof        TEXT    NOT NULL,"
          "  user_did     TEXT    NOT NULL,"
          "  user_name    TEXT    NOT NULL,"
          "  memo         TEXT    NOT NULL,"
          "  PRIMARY KEY(channel_id, post_id)"
          ")";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "Create tipping sqlite3_prepare_v2() failed");
        return -1;
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        vlogE(TAG_DB "Creating tipping table failed");
        return -1;
    }

    vlogD(TAG_DB "Create table tipping successfully");
    return 0;
}

static
int create_table_notification(void)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "CREATE TABLE IF NOT EXISTS notification ("
          "  notification_id INTEGER NOT NULL,"
          "  channel_id      INTEGER NOT NULL,"
          "  post_id         INTEGER NOT NULL,"
          "  comment_id      INTEGER NOT NULL,"
          "  action_type     INTEGER NOT NULL,"
          "  user_did        TEXT    NOT NULL,"
          "  user_name       TEXT    NOT NULL,"
          "  created_at      REAL    NOT NULL,"
          "  memo            TEXT    NOT NULL,"
          "  PRIMARY KEY(channel_id, post_id, comment_id)"
          ")";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "Create notification sqlite3_prepare_v2() failed");
        return -1;
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        vlogE(TAG_DB "Creating notification table failed");
        return -1;
    }

    vlogD(TAG_DB "Create table notification successfully");
    return 0;
}

int db_init(sqlite3 *handle)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    db = handle;

    /* ================== stmt-sep BEGIN ================== */
    sql = "BEGIN";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        vlogE(TAG_DB "BEGIN failed");
        return -1;
    }

    /* ================== channels table ================== */
    rc = check_table_valid("channels", 13);    //check table channels
    if (2 == rc) {   //table channels has 13 items
        //do nothing
    } else if (1 == rc) {    //table channels exist but version older
        vlogD(TAG_DB "Table channels is old version, updating");
        if (0 == backup_table("channels")) {    //backup channels successfully
            if (-1 == delete_old_index("channels")) {
                vlogE(TAG_DB "Delete table channels old index failed");
                goto rollback;
            }
            //create new version talbe channels
            if (-1 == create_table_channels()) {    //create table channels failed
                vlogE(TAG_DB "Create table channels DB failed");
                goto rollback;
            } else {    //create table channels successfully
                if (-1 == retrive_channels_data()) {    //sync table data from backup
                    vlogE(TAG_DB "Retrive table channels failed");
                    goto rollback;
                }
            }
        } else {    //backup channels failed
            vlogE(TAG_DB "Backup table channels DB failed");
            return -1;
        }
    } else if (0 == rc) {    //table channels dont exist, create it
        vlogD(TAG_DB "Table channels do not existed");
        if (-1 == create_table_channels()) {    //create table channels failed
            vlogE(TAG_DB "Create table channels DB failed");
            return -1;
        }
    } else {
        vlogE(TAG_DB "Check table channels exist DB failed");
        return -1;
    }


    /* ================== posts table ================== */
    rc = check_table_valid("posts", 14);    //check table posts
    if (2 == rc) {   //table posts has 14 items
        //do nothing
    } else if (1 == rc) {    //table posts exist but version older
        vlogD(TAG_DB "Table posts is old version, updating");
        if (0 == backup_table("posts")) {    //backup posts successfully
            if (-1 == delete_old_index("posts")) {
                vlogE(TAG_DB "Delete table posts old index failed");
                goto rollback;
            }
            //create new version talbe posts
            if (-1 == create_table_posts()) {    //create table posts failed
                vlogE(TAG_DB "Create table posts DB failed");
                goto rollback;
            } else {    //create table posts successfully
                if (-1 == retrive_posts_data()) {    //sync table data from backup
                    vlogE(TAG_DB "Retrive table posts failed");
                    goto rollback;
                }
            }
        } else {    //backup posts failed
            vlogE(TAG_DB "Backup table posts DB failed");
            return -1;
        }
    } else if (0 == rc) {    //table posts dont exist, create it
        vlogD(TAG_DB "Table posts do not existed");
        if (-1 == create_table_posts()) {    //create table posts failed
            vlogE(TAG_DB "Create table posts DB failed");
            return -1;
        }
    } else {
        vlogE(TAG_DB "Check table posts exist DB failed");
        return -1;
    }


    /* ================== comments table ================== */
    rc = check_table_valid("comments", 15);    //check table comments
    if (2 == rc) {   //table comments has 15 items
        //do nothing
    } else if (1 == rc) {    //table comments exist but version older
        vlogD(TAG_DB "Table comments is old version, updating");
        if (0 == backup_table("comments")) {    //backup comments successfully
            if (-1 == delete_old_index("comments")) {
                vlogE(TAG_DB "Delete table comments old index failed");
                goto rollback;
            }
            //create new version talbe comments
            if (-1 == create_table_comments()) {    //create table comments failed
                vlogE(TAG_DB "Create table comments DB failed");
                goto rollback;
            } else {    //create table comments successfully
                if (-1 == retrive_comments_data()) {    //sync table data from backup
                    vlogE(TAG_DB "Retrive table comments failed");
                    goto rollback;
                }
            }
        } else {    //backup comments failed
            vlogE(TAG_DB "Backup table comments DB failed");
            return -1;
        }
    } else if (0 == rc) {    //table comments dont exist, create it
        vlogD(TAG_DB "Table comments do not existed");
        if (-1 == create_table_comments()) {    //create table comments failed
            vlogE(TAG_DB "Create table comments DB failed");
            return -1;
        }
    } else {
        vlogE(TAG_DB "Check table comments exist DB failed");
        return -1;
    }


    /* ================== users table ================== */
    rc = check_table_valid("users", 8);    //check table users
    if (2 == rc) {   //table users has 8 items
        //do nothing
    } else if (1 == rc) {    //table users exist but version older
        vlogD(TAG_DB "Table users is old version, updating");
        if (0 == backup_table("users")) {    //backup users successfully
            //create new version talbe users
            if (-1 == create_table_users()) {    //create table users failed
                vlogE(TAG_DB "Create table users DB failed");
                goto rollback;
            } else {    //create table users successfully
                if (-1 == retrive_users_data()) {    //sync table data from backup
                    vlogE(TAG_DB "Retrive table users failed");
                    goto rollback;
                }
            }
        } else {    //backup users failed
            vlogE(TAG_DB "Backup table users DB failed");
            return -1;
        }
    } else if (0 == rc) {    //table users dont exist, create it
        vlogD(TAG_DB "Table users do not existed");
        if (-1 == create_table_users()) {    //create table users failed
            vlogE(TAG_DB "Create table users DB failed");
            return -1;
        }
    } else {
        vlogE(TAG_DB "Check table users exist DB failed");
        return -1;
    }


    /* ================== subscriptions table ================== */
    rc = check_table_valid("subscriptions", 5);    //check table subscriptions
    if (2 == rc) {   //table subscriptions has 5 items
        //do nothing
    } else if (1 == rc) {    //table subscriptions exist but version older
        vlogD(TAG_DB "Table subscriptions is old version, updating");
        if (0 == backup_table("subscriptions")) {    //backup subscriptions successfully
            //create new version talbe subscriptions
            if (-1 == create_table_subscriptions()) {    //create table subscriptions failed
                vlogE(TAG_DB "Create table subscriptions DB failed");
                goto rollback;
            } else {    //create table subscriptions successfully
                if (-1 == retrive_subscriptions_data()) {    //sync table data from backup
                    vlogE(TAG_DB "Retrive table subscriptions failed");
                    goto rollback;
                }
            }
        } else {    //backup subscriptions failed
            vlogE(TAG_DB "Backup table subscriptions DB failed");
            return -1;
        }
    } else if (0 == rc) {    //table subscriptions dont exist, create it
        vlogD(TAG_DB "Table subscriptions do not existed");
        if (-1 == create_table_subscriptions()) {    //create table subscriptions failed
            vlogE(TAG_DB "Create table subscriptions DB failed");
            return -1;
        }
    } else {
        vlogE(TAG_DB "Check table subscriptions exist DB failed");
        return -1;
    }


    /* ================== likes table ================== */
    rc = check_table_valid("likes", 7);    //check table likes
    if (2 == rc) {   //table likes has 7 items
        //do nothing
    } else if (1 == rc) {    //table likes exist but version older
        vlogD(TAG_DB "Table likes is old version, updating");
        if (0 == backup_table("likes")) {    //backup likes successfully
            //create new version talbe likes
            if (-1 == create_table_likes()) {    //create table likes failed
                vlogE(TAG_DB "Create table likes DB failed");
                goto rollback;
            } else {    //create table likes successfully
                if (-1 == retrive_likes_data()) {    //sync table data from backup
                    vlogE(TAG_DB "Retrive table likes failed");
                    goto rollback;
                }
            }
        } else {    //backup likes failed
            vlogE(TAG_DB "Backup table likes DB failed");
            return -1;
        }
    } else if (0 == rc) {    //table likes dont exist, create it
        vlogD(TAG_DB "Table likes do not existed");
        if (-1 == create_table_likes()) {    //create table likes failed
            vlogE(TAG_DB "Create table likes DB failed");
            return -1;
        }
    } else {
        vlogE(TAG_DB "Check table likes exist DB failed");
        return -1;
    }


    /* ================== reported_comments table ================== */
    rc = check_table_valid("reported_comments", 6);    //check table reported_comments
    if (2 == rc) {   //table reported_comments has 6 items
        //do nothing
    } else if (1 == rc) {    //table reported_comments exist but version older
        //not should be here
        return -1;
    } else if (0 == rc) {    //table reported_comments dont exist, create it
        vlogD(TAG_DB "Table reported_comments do not existed");
        if (-1 == create_table_reported_comments()) {    //create table reported_comments failed
            vlogE(TAG_DB "Create table reported_comments DB failed");
            return -1;
        }
    } else {
        vlogE(TAG_DB "Check table reported_comments exist DB failed");
        return -1;
    }


    /* ================== tipping table ================== */
    rc = check_table_valid("tipping", 10);    //check table tipping
    if (2 == rc) {   //table tipping has 10 items
        //do nothing
    } else if (1 == rc) {    //table tipping exist but version older
        //not should be here
        return -1;
    } else if (0 == rc) {    //table tipping dont exist, create it
        vlogD(TAG_DB "Table tipping do not existed");
        if (-1 == create_table_tipping()) {    //create table tipping failed
            vlogE(TAG_DB "Create table tipping DB failed");
            return -1;
        }
    } else {
        vlogE(TAG_DB "Check table tipping exist DB failed");
        return -1;
    }


    /* ================== notification table ================== */
    rc = check_table_valid("notification", 9);    //check table notification
    if (2 == rc) {   //table notification has 9 items
        //do nothing
    } else if (1 == rc) {    //table notification exist but version older
        //not should be here
        return -1;
    } else if (0 == rc) {    //table notification dont exist, create it
        vlogD(TAG_DB "Table notification do not existed");
        if (-1 == create_table_notification()) {    //create table notification failed
            vlogE(TAG_DB "Create table notification DB failed");
            return -1;
        }
    } else {
        vlogE(TAG_DB "Check table notification exist DB failed");
        return -1;
    }


    /* ================== stmt-sep END ================== */
    sql = "END";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        goto rollback;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        vlogE(TAG_DB "Executing END failed");
        goto rollback;
    }

    return 0;

rollback:
    sql = "ROLLBACK";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc) {
        vlogE(TAG_DB "ROLLBACK sqlite3_prepare_v2() failed");
        return -1;
    }

    sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        vlogE(TAG_DB "ROLLBACK failed");
    }

    return -1;
}

int db_create_chan(const ChanInfo *ci)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "INSERT INTO channels(created_at, updated_at, name, intro, avatar) "
          "  VALUES (:ts, :ts, :name, :intro, :avatar)";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_bind_int64(stmt,
                            sqlite3_bind_parameter_index(stmt, ":ts"),
                            ci->created_at);
    rc |= sqlite3_bind_text(stmt,
                            sqlite3_bind_parameter_index(stmt, ":name"),
                            ci->name, -1, NULL);
    rc |= sqlite3_bind_text(stmt,
                            sqlite3_bind_parameter_index(stmt, ":intro"),
                            ci->intro, -1, NULL);
    rc |= sqlite3_bind_blob(stmt,
                            sqlite3_bind_parameter_index(stmt, ":avatar"),
                            ci->avatar, ci->len, NULL);
    if (SQLITE_OK != rc) {
        vlogE(TAG_DB "Binding parameter failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "Inserting new channel failed");
        return -1;
    }

    return 0;
}

int db_upd_chan(const ChanInfo *ci)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "UPDATE channels"
          "  SET updated_at = :upd_at, name = :name, intro = :intro, avatar = :avatar"
          "  WHERE channel_id = :channel_id";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_bind_int64(stmt,
                            sqlite3_bind_parameter_index(stmt, ":upd_at"),
                            ci->upd_at);
    rc |= sqlite3_bind_text(stmt,
                           sqlite3_bind_parameter_index(stmt, ":name"),
                           ci->name, -1, NULL);
    rc |= sqlite3_bind_text(stmt,
                           sqlite3_bind_parameter_index(stmt, ":intro"),
                           ci->intro, -1, NULL);
    rc |= sqlite3_bind_blob(stmt,
                           sqlite3_bind_parameter_index(stmt, ":avatar"),
                           ci->avatar, ci->len, NULL);
    rc |= sqlite3_bind_int64(stmt,
                            sqlite3_bind_parameter_index(stmt, ":channel_id"),
                            ci->chan_id);
    if (SQLITE_OK != rc) {
        vlogE(TAG_DB "Binding parameter failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "Updating new channel failed");
        return -1;
    }

    return 0;
}

int db_add_post(const PostInfo *pi)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "BEGIN";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "Executing BEIGIN failed");
        return -1;
    }

    do {
        sql = "INSERT INTO posts(channel_id, post_id, created_at, updated_at, content, status) "
            "  VALUES (:channel_id, :post_id, :ts, :ts, :content, :status)";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                pi->chan_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":post_id"),
                pi->post_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":ts"),
                pi->created_at);
        rc |= sqlite3_bind_blob(stmt,
                sqlite3_bind_parameter_index(stmt, ":content"),
                pi->content, pi->len, NULL);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":status"),
                pi->stat);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing INSERT failed");
            break;
        }

        sql = "UPDATE channels "
            "  SET next_post_id = next_post_id + 1"
            "  WHERE channel_id = :channel_id";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                pi->chan_id);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter channel_id failed");
            sqlite3_finalize(stmt);
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing UPDATE failed");
            break;
        }

        sql = "END";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing END failed");
            break;
        }

        return 0;
    } while(0);

    sql = "ROLLBACK";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "ROLLBACK failed");
    }

    return -1;
}

int db_post_is_avail(uint64_t chan_id, uint64_t post_id)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "SELECT EXISTS(SELECT *"
          "                FROM posts"
          "                WHERE channel_id = :channel_id AND"
          "                      post_id = :post_id AND"
          "                      status = :avail)";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_bind_int64(stmt,
                            sqlite3_bind_parameter_index(stmt, ":channel_id"),
                            chan_id);
    rc |= sqlite3_bind_int64(stmt,
                            sqlite3_bind_parameter_index(stmt, ":post_id"),
                            post_id);
    rc |= sqlite3_bind_int64(stmt,
                            sqlite3_bind_parameter_index(stmt, ":avail"),
                            POST_AVAILABLE);
    if (SQLITE_OK != rc) {
        vlogE(TAG_DB "Binding parameter failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    if (SQLITE_ROW != sqlite3_step(stmt)) {
        vlogE(TAG_DB "Executing SELECT failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    rc = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    return rc ? 1 : 0;
}

int db_upd_post(PostInfo *pi)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "BEGIN";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "Executing BEIGIN failed");
        return -1;
    }

    do {
        sql = "UPDATE posts"
              "  SET updated_at = :upd_at, content = :content"
              "  WHERE channel_id = :channel_id AND post_id = :post_id";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":upd_at"),
                pi->upd_at);
        rc |= sqlite3_bind_blob(stmt,
                sqlite3_bind_parameter_index(stmt, ":content"),
                pi->content, pi->len, NULL);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                pi->chan_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":post_id"),
                pi->post_id);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing UPDATE failed");
            break;
        }

        sql = "SELECT next_comment_id - 1 AS comments, likes, created_at"
              "  FROM posts"
              "  WHERE channel_id = :channel_id AND post_id = :post_id";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                pi->chan_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":post_id"),
                pi->post_id);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        rc = sqlite3_step(stmt);
        if (SQLITE_ROW != rc) {
            vlogE(TAG_DB "Executing SELECT failed");
            sqlite3_finalize(stmt);
            break;
        }

        pi->cmts       = sqlite3_column_int64(stmt, 0);
        pi->likes      = sqlite3_column_int64(stmt, 1);
        pi->created_at = sqlite3_column_int64(stmt, 2);
        sqlite3_finalize(stmt);

        sql = "END";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing END failed");
            break;
        }

        return 0;
    } while(0);

    sql = "ROLLBACK";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "ROLLBACK failed");
    }

    return -1;
}

int db_set_post_status(PostInfo *pi)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "BEGIN";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "Executing BEGIN failed");
        return -1;
    }

    do {
        sql = "UPDATE posts"
              "  SET status = :status"
              "  WHERE channel_id = :channel_id AND post_id = :post_id";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":status"),
                pi->stat);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                pi->chan_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":post_id"),
                pi->post_id);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing UPDATE failed");
            break;
        }

        sql = "SELECT next_comment_id - 1 AS comments, likes, created_at"
              "  FROM posts"
              "  WHERE channel_id = :channel_id AND post_id = :post_id";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                pi->chan_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":post_id"),
                pi->post_id);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        if (SQLITE_ROW != sqlite3_step(stmt)) {
            vlogE(TAG_DB "Executing SELECT failed");
            sqlite3_finalize(stmt);
            break;
        }

        pi->cmts       = sqlite3_column_int64(stmt, 0);
        pi->likes      = sqlite3_column_int64(stmt, 1);
        pi->created_at = sqlite3_column_int64(stmt, 2);
        sqlite3_finalize(stmt);

        sql = "END";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing END failed");
            break;
        }

        return 0;
    } while(0);

    sql = "ROLLBACK";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "ROLLBACK failed");
    }

    return -1;
}

int db_cmt_exists(uint64_t channel_id, uint64_t post_id, uint64_t comment_id)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "SELECT EXISTS( "
          "  SELECT * "
          "    FROM comments "
          "    WHERE channel_id = :channel_id AND "
          "          post_id = :post_id AND "
          "          comment_id = :comment_id"
          ")";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_bind_int64(stmt,
                            sqlite3_bind_parameter_index(stmt, ":channel_id"),
                            channel_id);
    rc |= sqlite3_bind_int64(stmt,
                            sqlite3_bind_parameter_index(stmt, ":post_id"),
                            post_id);
    rc |= sqlite3_bind_int64(stmt,
                            sqlite3_bind_parameter_index(stmt, ":comment_id"),
                            comment_id);
    if (SQLITE_OK != rc) {
        vlogE(TAG_DB "Binding parameter failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    if (SQLITE_ROW != sqlite3_step(stmt)) {
        vlogE(TAG_DB "Executing SELECT failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    rc = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    return rc ? 1 : 0;
}

int db_cmt_is_avail(uint64_t channel_id, uint64_t post_id, uint64_t comment_id)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "SELECT EXISTS(SELECT *"
          "                FROM comments"
          "                WHERE channel_id = :channel_id AND"
          "                      post_id = :post_id AND"
          "                      comment_id = :comment_id AND"
          "                      status = :avail)";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_bind_int64(stmt,
                            sqlite3_bind_parameter_index(stmt, ":channel_id"),
                            channel_id);
    rc |= sqlite3_bind_int64(stmt,
                            sqlite3_bind_parameter_index(stmt, ":post_id"),
                            post_id);
    rc |= sqlite3_bind_int64(stmt,
                            sqlite3_bind_parameter_index(stmt, ":comment_id"),
                            comment_id);
    rc |= sqlite3_bind_int64(stmt,
                            sqlite3_bind_parameter_index(stmt, ":avail"),
                            CMT_AVAILABLE);
    if (SQLITE_OK != rc) {
        vlogE(TAG_DB "Binding parameter failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    if (SQLITE_ROW != sqlite3_step(stmt)) {
        vlogE(TAG_DB "Executing SELECT failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    rc = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    return rc ? 1 : 0;
}

int db_cmt_uid(uint64_t channel_id, uint64_t post_id, uint64_t comment_id, uint64_t *uid)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "SELECT user_id"
          "  FROM comments"
          "  WHERE channel_id = :channel_id AND"
          "        post_id = :post_id AND"
          "        comment_id = :comment_id";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_bind_int64(stmt,
            sqlite3_bind_parameter_index(stmt, ":channel_id"),
            channel_id);
    rc |= sqlite3_bind_int64(stmt,
            sqlite3_bind_parameter_index(stmt, ":post_id"),
            post_id);
    rc |= sqlite3_bind_int64(stmt,
            sqlite3_bind_parameter_index(stmt, ":comment_id"),
            comment_id);
    if (SQLITE_OK != rc) {
        vlogE(TAG_DB "Binding parameter failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    if (SQLITE_ROW != sqlite3_step(stmt)) {
        vlogE(TAG_DB "Executing SELECT failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    *uid = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);

    return 0;
}

int db_add_cmt(CmtInfo *ci, uint64_t *id)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "BEGIN";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "Executing BEGIN failed");
        return -1;
    }

    do {
        sql = "INSERT INTO comments("
              "  channel_id, post_id, comment_id, "
              "  refcomment_id, user_id, created_at, updated_at, content"
              ") VALUES ("
              "  :channel_id, :post_id, "
              "  (SELECT next_comment_id "
              "     FROM posts "
              "     WHERE channel_id = :channel_id AND "
              "           post_id = :post_id), "
              "  :comment_id, :uid, :ts, :ts, :content"
              ")";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                ci->chan_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":post_id"),
                ci->post_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":comment_id"),
                ci->reply_to_cmt);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":uid"),
                ci->user.uid);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":ts"),
                ci->created_at);
        rc |= sqlite3_bind_blob(stmt,
                sqlite3_bind_parameter_index(stmt, ":content"),
                ci->content, ci->len, NULL);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing INSERT failed");
            break;
        }

        sql = "UPDATE posts "
              "  SET next_comment_id = next_comment_id + 1 "
              "  WHERE channel_id = :channel_id AND post_id = :post_id";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                ci->chan_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":post_id"),
                ci->post_id);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing UPDATE failed");
            break;
        }

        sql = "SELECT next_comment_id - 1 "
              "  FROM posts "
              "  WHERE channel_id = :channel_id AND post_id = :post_id";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                ci->chan_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":post_id"),
                ci->post_id);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        if (SQLITE_ROW != sqlite3_step(stmt)) {
            vlogE(TAG_DB "Executing SELECT failed");
            sqlite3_finalize(stmt);
            break;
        }

        *id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);

        sql = "END";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing END failed");
            break;
        }

        return 0;
    } while(0);

    sql = "ROLLBACK";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "ROLLBACK failed");
    }

    return -1;
}

int db_get_post_status(uint64_t chan_id, uint64_t post_id)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;
    PostStat stat;

    sql = "BEGIN";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "Executing BEGIN failed");
        return -1;
    }

    do {
        sql = "SELECT status"
              "  FROM posts"
              "  WHERE channel_id = :channel_id AND post_id = :post_id";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                chan_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":post_id"),
                post_id);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        if (SQLITE_ROW != sqlite3_step(stmt)) {
            vlogE(TAG_DB "Executing SELECT failed");
            sqlite3_finalize(stmt);
            break;
        }

        stat = (PostStat)sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);

        sql = "END";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing END failed");
            break;
        }

        return stat;
    } while(0);

    sql = "ROLLBACK";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "ROLLBACK failed");
    }

    return -1;
}

int db_get_post(uint64_t chan_id, uint64_t post_id, PostInfo *pi)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "BEGIN";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "Executing BEGIN failed");
        return -1;
    }

    do {
        sql = "SELECT status, content, length(content), "
              "       next_comment_id - 1 AS comments, likes, created_at, updated_at "
              "  FROM posts "
              "  WHERE channel_id = :channel_id AND post_id = :post_id";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                chan_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":post_id"),
                post_id);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        if (SQLITE_ROW != sqlite3_step(stmt)) {
            vlogE(TAG_DB "Executing SELECT failed");
            sqlite3_finalize(stmt);
            break;
        }

        pi->stat = (PostStat)sqlite3_column_int64(stmt, 0);
        const void* content = sqlite3_column_blob(stmt, 1); 
        int content_size = sqlite3_column_int64(stmt, 2); 
        pi->content = rc_zalloc(content_size, NULL);
        memcpy(pi->content, content, content_size);
        pi->len = content_size;
        pi->cmts = sqlite3_column_int64(stmt, 3); 
        pi->likes = sqlite3_column_int64(stmt, 4); 
        pi->created_at = sqlite3_column_int64(stmt, 5); 
        pi->upd_at = sqlite3_column_int64(stmt, 6); 
        sqlite3_finalize(stmt);

        sql = "END";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing END failed");
            break;
        }

        return 0;
    } while(0);

    sql = "ROLLBACK";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "ROLLBACK failed");
    }

    return -1;
}

int db_upd_cmt(CmtInfo *ci)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "BEGIN";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "Executing BEGIN failed");
        return -1;
    }

    do {
        sql = "UPDATE comments"
              "  SET updated_at = :upd_at, content = :content, refcomment_id = :ref_cmt_id"
              "  WHERE channel_id = :channel_id AND post_id = :post_id AND comment_id = :comment_id";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":upd_at"),
                ci->upd_at);
        rc |= sqlite3_bind_blob(stmt,
                sqlite3_bind_parameter_index(stmt, ":content"),
                ci->content, ci->len, NULL);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":ref_cmt_id"),
                ci->reply_to_cmt);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                ci->chan_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":post_id"),
                ci->post_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":comment_id"),
                ci->cmt_id);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing UPDATE failed");
            break;
        }

        sql = "SELECT likes, created_at"
              "  FROM comments"
              "  WHERE channel_id = :channel_id AND post_id = :post_id AND comment_id = :comment_id";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                ci->chan_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":post_id"),
                ci->post_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":comment_id"),
                ci->cmt_id);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        if (SQLITE_ROW != sqlite3_step(stmt)) {
            vlogE(TAG_DB "Executing SELECT failed");
            sqlite3_finalize(stmt);
            break;
        }

        ci->likes      = sqlite3_column_int64(stmt, 0);
        ci->created_at = sqlite3_column_int64(stmt, 1);
        sqlite3_finalize(stmt);


        sql = "END";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing END failed");
            break;
        }

        return 0;
    } while(0);

    sql = "ROLLBACK";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "ROLLBACK failed");
    }

    return -1;
}

int db_set_cmt_status(CmtInfo *ci)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "BEGIN";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "Executing BEGIN failed");
        return -1;
    }

    do {
        sql = "UPDATE comments"
              "  SET updated_at = :upd_at, status = :deleted"
              "  WHERE channel_id = :channel_id AND post_id = :post_id AND comment_id = :comment_id";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":upd_at"),
                ci->upd_at);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":deleted"),
                ci->stat);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                ci->chan_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":post_id"),
                ci->post_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":comment_id"),
                ci->cmt_id);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing UPDATE failed");
            break;
        }

        sql = "SELECT refcomment_id, likes, created_at"
              "  FROM comments"
              "  WHERE channel_id = :channel_id AND post_id = :post_id AND comment_id = :comment_id";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                ci->chan_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":post_id"),
                ci->post_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":comment_id"),
                ci->cmt_id);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        if (SQLITE_ROW != sqlite3_step(stmt)) {
            vlogE(TAG_DB "Executing SELECT failed");
            sqlite3_finalize(stmt);
            break;
        }

        ci->reply_to_cmt = sqlite3_column_int64(stmt, 0);
        ci->likes        = sqlite3_column_int64(stmt, 1);
        ci->created_at   = sqlite3_column_int64(stmt, 2);
        sqlite3_finalize(stmt);

        sql = "END";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing END failed");
            break;
        }

        return 0;
    } while(0);

    sql = "ROLLBACK";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "ROLLBACK failed");
    }

    return -1;
}

int db_like_exists(uint64_t uid, uint64_t channel_id, uint64_t post_id, uint64_t comment_id)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "SELECT EXISTS(SELECT * "
          "                FROM likes "
          "                WHERE user_id = :uid AND "
          "                      channel_id = :channel_id AND "
          "                      post_id = :post_id AND "
          "                      comment_id = :comment_id)";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_bind_int64(stmt,
            sqlite3_bind_parameter_index(stmt, ":uid"),
            uid);
    rc |= sqlite3_bind_int64(stmt,
            sqlite3_bind_parameter_index(stmt, ":channel_id"),
            channel_id);
    rc |= sqlite3_bind_int64(stmt,
            sqlite3_bind_parameter_index(stmt, ":post_id"),
            post_id);
    rc |= sqlite3_bind_int64(stmt,
            sqlite3_bind_parameter_index(stmt, ":comment_id"),
            comment_id);
    if (SQLITE_OK != rc) {
        vlogE(TAG_DB "Binding parameter failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    if (SQLITE_ROW != sqlite3_step(stmt)) {
        vlogE(TAG_DB "Executing SELECT failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    rc = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    return rc ? 1 : 0;
}

int db_add_like(uint64_t uid, uint64_t channel_id, uint64_t post_id,
        uint64_t comment_id, uint64_t *likes)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "BEGIN";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "Exectuing BEGIN failed");
        return -1;
    }

    do {
        sql = comment_id ?
            "UPDATE comments "
            "  SET likes = likes + 1 "
            "  WHERE channel_id = :channel_id AND "
            "        post_id = :post_id AND "
            "        comment_id = :comment_id" :
            "UPDATE posts "
            "  SET likes = likes + 1 "
            "  WHERE channel_id = :channel_id AND "
            "        post_id = :post_id";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                channel_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":post_id"),
                post_id);
        if (comment_id) {
            rc |= sqlite3_bind_int64(stmt,
                    sqlite3_bind_parameter_index(stmt, ":comment_id"),
                    comment_id);
        }
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing UPDATE failed");
            break;
        }

        sql = "INSERT INTO likes(user_id, channel_id, post_id, comment_id, created_at) "
              "  VALUES (:uid, :channel_id, :post_id, :comment_id, :ts)";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":uid"),
                uid);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                channel_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":post_id"),
                post_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":comment_id"),
                comment_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":ts"),
                time(NULL));
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing INSERT failed");
            break;
        }

        sql = comment_id ?
            "SELECT likes "
            "  FROM comments "
            "  WHERE channel_id = :channel_id AND "
            "        post_id = :post_id AND "
            "        comment_id = :comment_id" :
            "SELECT likes "
            "  FROM posts "
            "  WHERE channel_id = :channel_id AND "
            "        post_id = :post_id";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                channel_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":post_id"),
                post_id);
        if (comment_id) {
            rc |= sqlite3_bind_int64(stmt,
                    sqlite3_bind_parameter_index(stmt, ":comment_id"),
                    comment_id);
        }
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        if (SQLITE_ROW != sqlite3_step(stmt)) {
            vlogE(TAG_DB "Executing SELECT failed");
            sqlite3_finalize(stmt);
            break;
        }

        *likes = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);

        sql = "END";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing END failed");
            break;
        }

        return 0;
    } while(0);

    sql = "ROLLBACK";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "ROLLBACK failed");
    }

    return -1;
}

int db_rm_like(uint64_t uid, uint64_t channel_id, uint64_t post_id, uint64_t comment_id)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "BEGIN";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "Executing BEGIN failed");
        return -1;
    }

    do {
        sql = comment_id ?
            "UPDATE comments "
            "  SET likes = likes - 1 "
            "  WHERE channel_id = :channel_id AND "
            "        post_id = :post_id AND "
            "        comment_id = :comment_id" :
            "UPDATE posts "
            "  SET likes = likes - 1 "
            "  WHERE channel_id = :channel_id AND "
            "        post_id = :post_id";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                channel_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":post_id"),
                post_id);
        if (comment_id) {
            rc |= sqlite3_bind_int64(stmt,
                    sqlite3_bind_parameter_index(stmt, ":comment_id"),
                    comment_id);
        }
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing UPDATE failed");
            break;
        }

        sql = "DELETE FROM likes "
              "  WHERE user_id = :uid AND channel_id = :channel_id AND "
              "        post_id = :post_id AND comment_id = :comment_id";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":uid"),
                uid);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                channel_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":post_id"),
                post_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":comment_id"),
                comment_id);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing DELETE failed");
            break;
        }

        sql = "END";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing END failed");
            break;
        }

        return 0;
    } while(0);

    sql = "ROLLBACK";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "ROLLBACK failed");
    }

    return -1;
}

int db_add_sub(uint64_t uid, uint64_t chan_id)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "BEGIN";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "Executing BEGIN failed");
        return -1;
    }

    do {
        sql = "INSERT INTO subscriptions(user_id, channel_id) "
              "  VALUES (:uid, :channel_id)";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":uid"),
                uid);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                chan_id);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing INSERT failed");
            break;
        }

        sql = "UPDATE channels "
              "  SET subscribers = subscribers + 1"
              "  WHERE channel_id = :channel_id";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                chan_id);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing UPDATE failed");
            break;
        }

        sql = "END";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing END failed");
            break;
        }

        return 0;
    } while(0);

    sql = "ROLLBACK";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "ROLLBACK failed");
    }

    return -1;
}

int db_unsub(uint64_t uid, uint64_t chan_id)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "BEGIN";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "Executing BEGIN failed");
        return -1;
    }

    do {
        sql = "DELETE FROM subscriptions "
              "  WHERE user_id = :uid AND channel_id = :channel_id";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":uid"),
                uid);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                chan_id);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing DELETE failed");
            break;
        }

        sql = "UPDATE channels "
              "  SET subscribers = subscribers - 1"
              "  WHERE channel_id = :channel_id";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                chan_id);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing UPDATE failed");
            break;
        }

        sql = "END";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing END failed");
            break;
        }

        return 0;
    } while(0);

    sql = "ROLLBACK";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "ROLLBACK failed");
    }

    return -1;
}

int db_upsert_user(const UserInfo *ui, uint64_t *uid)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "INSERT INTO users(did, name, email) VALUES (:did, :name, :email) "
          "  ON CONFLICT (did) "
          "  DO UPDATE "
          "       SET name = :name, email = :email "
          "       WHERE excluded.name IS NOT name OR excluded.email IS NOT email";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_bind_text(stmt,
            sqlite3_bind_parameter_index(stmt, ":did"),
            ui->did, -1, NULL);
    rc |= sqlite3_bind_text(stmt,
            sqlite3_bind_parameter_index(stmt, ":name"),
            ui->name, -1, NULL);
    rc |= sqlite3_bind_text(stmt,
            sqlite3_bind_parameter_index(stmt, ":email"),
            ui->email, -1, NULL);
    if (SQLITE_OK != rc) {
        vlogE(TAG_DB "Binding parameter failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "Executing INSERT failed");
        return -1;
    }

    sql = "SELECT user_id FROM users WHERE did = :did";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_bind_text(stmt,
            sqlite3_bind_parameter_index(stmt, ":did"),
            ui->did, -1, NULL);
    if (SQLITE_OK != rc) {
        vlogE(TAG_DB "Binding parameter did failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    if (SQLITE_ROW != sqlite3_step(stmt)) {
        vlogE(TAG_DB "Executing SELECT failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    *uid = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);

    return 0;
}

typedef enum {
    CHANNEL,
    POST,
    COMMENT
} QueryObject;

static
const char *query_column(QueryObject qo, QryFld qf)
{
    switch (qf) {
        case ID:
            switch (qo) {
                case CHANNEL:
                    return "channel_id";
                case POST:
                    return "post_id";
                case COMMENT:
                    return "comment_id";
                default:
                    assert(0);
                    return NULL;
            }
        case UPD_AT:
            return "updated_at";
        case CREATED_AT:
            return "created_at";
        default:
            assert(0);
            return NULL;
    }
}

static
void it_dtor(void *obj)
{
    DBObjIt *it = (DBObjIt *)obj;

    if (it->stmt)
        sqlite3_finalize(it->stmt);
}

static
DBObjIt *it_create(sqlite3_stmt *stmt, Row2Raw cb)
{
    DBObjIt *it = (DBObjIt *)rc_zalloc(sizeof(DBObjIt), it_dtor);
    if (!it)
        return NULL;

    it->stmt = stmt;
    it->cb   = cb;

    return it;
}

static
void *row2chan(sqlite3_stmt *stmt)
{
    const char *name = (const char *)sqlite3_column_text(stmt, 1);
    const char *intro = (const char *)sqlite3_column_text(stmt, 2);
    size_t avatar_sz = sqlite3_column_int64(stmt, 8);
    ChanInfo *ci = (ChanInfo *)rc_zalloc(sizeof(ChanInfo) + strlen(name) + strlen(intro) + 2 + avatar_sz, NULL);
    void *buf;

    if (!ci) {
        vlogE(TAG_DB "OOM");
        return NULL;
    }

    buf = ci + 1;

    ci->chan_id      = sqlite3_column_int64(stmt, 0);
    ci->name         = strcpy((char *)buf, name);
    buf += strlen(name) + 1;
    ci->intro        = strcpy((char *)buf, intro);
    buf += strlen(intro) + 1;
    ci->subs         = sqlite3_column_int64(stmt, 3);
    ci->next_post_id = sqlite3_column_int64(stmt, 4);
    ci->upd_at       = sqlite3_column_int64(stmt, 5);
    ci->created_at   = sqlite3_column_int64(stmt, 6);
    ci->owner        = &feeds_owner_info;
    ci->avatar       = memcpy(buf, sqlite3_column_blob(stmt, 7), avatar_sz);
    ci->len          = avatar_sz;

    return ci;
}

DBObjIt *db_iter_chans(const QryCriteria *qc)
{
    sqlite3_stmt *stmt;
    const char *qcol;
    char sql[1024] = {0};
    DBObjIt *it;
    int rc;

    rc = sprintf(sql,
                 "SELECT channel_id, name, intro, subscribers, "
                 "       next_post_id, updated_at, created_at, avatar, length(avatar) "
                 "  FROM channels");
    if (qc->by) {
        qcol = query_column(CHANNEL, (QryFld)qc->by);
        if (qc->lower || qc->upper)
            rc += sprintf(sql + rc, " WHERE ");
        if (qc->lower)
            rc += sprintf(sql + rc, "%s >= :lower", qcol);
        if (qc->upper)
            rc += sprintf(sql + rc, "%s %s <= :upper", qc->lower ? " AND" : "", qcol);
        rc += sprintf(sql + rc, " ORDER BY %s %s", qcol, qc->by == ID ? "ASC" : "DESC");
    }
    if (qc->maxcnt)
        rc += sprintf(sql + rc, " LIMIT :maxcnt");

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return NULL;
    }

    if (qc->lower) {
        rc = sqlite3_bind_int64(stmt,
                                sqlite3_bind_parameter_index(stmt, ":lower"),
                                qc->lower);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter lower failed");
            sqlite3_finalize(stmt);
            return NULL;
        }
    }

    if (qc->upper) {
        rc = sqlite3_bind_int64(stmt,
                                sqlite3_bind_parameter_index(stmt, ":upper"),
                                qc->upper);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter upper failed");
            sqlite3_finalize(stmt);
            return NULL;
        }
    }

    if (qc->maxcnt) {
        rc = sqlite3_bind_int64(stmt,
                                sqlite3_bind_parameter_index(stmt, ":maxcnt"),
                                qc->maxcnt);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter maxcnt failed");
            sqlite3_finalize(stmt);
            return NULL;
        }
    }

    it = it_create(stmt, row2chan);
    if (!it) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    return it;
}

static
void *row2subchan(sqlite3_stmt *stmt)
{
    const char *name = (const char *)sqlite3_column_text(stmt, 1);
    const char *intro = (const char *)sqlite3_column_text(stmt, 2);
    size_t avatar_sz = sqlite3_column_int64(stmt, 6);
    ChanInfo *ci = (ChanInfo *)rc_zalloc(sizeof(ChanInfo) + strlen(name) + strlen(intro) + 2 + avatar_sz, NULL);
    void *buf;

    if (!ci) {
        vlogE(TAG_DB "OOM");
        return NULL;
    }

    buf = ci + 1;
    ci->chan_id = sqlite3_column_int64(stmt, 0);
    ci->name    = strcpy((char *)buf, name);
    buf += strlen(name) + 1;
    ci->intro   = strcpy((char *)buf, intro);
    buf += strlen(intro) + 1;
    ci->subs    = sqlite3_column_int64(stmt, 3);
    ci->upd_at  = sqlite3_column_int64(stmt, 4);
    ci->owner   = &feeds_owner_info;
    ci->avatar  = memcpy(buf, sqlite3_column_blob(stmt, 5), avatar_sz);
    ci->len     = avatar_sz;

    return ci;
}

DBObjIt *db_iter_sub_chans(uint64_t uid, const QryCriteria *qc)
{
    sqlite3_stmt *stmt;
    const char *qcol;
    char sql[1024] = {0};
    DBObjIt *it;
    int rc;

    rc = sprintf(sql,
                 "SELECT channel_id, name, intro, subscribers, updated_at, avatar, length(avatar) "
                 "  FROM (SELECT channel_id "
                 "          FROM subscriptions "
                 "          WHERE user_id = :uid) JOIN channels USING (channel_id)");

    if (qc->by) {
        qcol = query_column(CHANNEL, (QryFld)qc->by);
        if (qc->lower || qc->upper)
            rc += sprintf(sql + rc, " WHERE ");
        if (qc->lower)
            rc += sprintf(sql + rc, "%s >= :lower", qcol);
        if (qc->upper)
            rc += sprintf(sql + rc, "%s %s <= :upper", qc->lower ? " AND" : "", qcol);
        rc += sprintf(sql + rc, " ORDER BY %s %s", qcol, qc->by == ID ? "ASC" : "DESC");
    }
    if (qc->maxcnt)
        rc += sprintf(sql +rc, " LIMIT :maxcnt");

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return NULL;
    }

    rc = sqlite3_bind_int64(stmt,
                            sqlite3_bind_parameter_index(stmt, ":uid"),
                            uid);
    if (qc->lower) {
        rc |= sqlite3_bind_int64(stmt,
                                sqlite3_bind_parameter_index(stmt, ":lower"),
                                qc->lower);
    }
    if (qc->upper) {
        rc |= sqlite3_bind_int64(stmt,
                                sqlite3_bind_parameter_index(stmt, ":upper"),
                                qc->upper);
    }
    if (qc->maxcnt) {
        rc |= sqlite3_bind_int64(stmt,
                                sqlite3_bind_parameter_index(stmt, ":maxcnt"),
                                qc->maxcnt);
    }
    if (SQLITE_OK != rc) {
        vlogE(TAG_DB "Binding parameter failed");
        sqlite3_finalize(stmt);
        return NULL;
    }

    it = it_create(stmt, row2subchan);
    if (!it) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    return it;
}

static
void *row2post(sqlite3_stmt *stmt)
{
    PostStat stat = (PostStat)sqlite3_column_int64(stmt, 2);
    size_t len = (stat != POST_AVAILABLE ? 0 : sqlite3_column_int64(stmt, 4));
    PostInfo *pi;
    void *buf;

    pi = (PostInfo *)rc_zalloc(sizeof(PostInfo) + len, NULL);
    if (!pi) {
        vlogE(TAG_DB "OOM");
        return NULL;
    }

    pi->chan_id     = sqlite3_column_int64(stmt, 0);
    pi->post_id     = sqlite3_column_int64(stmt, 1);
    pi->stat        = stat;
    if (stat == POST_AVAILABLE) {
        buf = pi + 1;
        pi->content = memcpy(buf, sqlite3_column_blob(stmt, 3), len);
        pi->len     = len;
    }
    pi->cmts        = sqlite3_column_int64(stmt, 5);
    pi->likes       = sqlite3_column_int64(stmt, 6);
    pi->created_at  = sqlite3_column_int64(stmt, 7);
    pi->upd_at      = sqlite3_column_int64(stmt, 8);

    return pi;
}

DBObjIt *db_iter_posts(uint64_t chan_id, const QryCriteria *qc)
{
    sqlite3_stmt *stmt;
    const char *qcol;
    char sql[1024] = {0};
    DBObjIt *it;
    int rc;

    rc = sprintf(sql,
                 "SELECT channel_id, post_id, status, content, length(content), "
                 "       next_comment_id - 1 AS comments, likes, created_at, updated_at "
                 "  FROM posts "
                 "  WHERE channel_id = :channel_id");
    rc += sprintf(sql + rc, " AND (status=%d OR status=%d)", POST_AVAILABLE, POST_DELETED);
    if (qc->by) {
        qcol = query_column(POST, (QryFld)qc->by);
        if (qc->lower)
            rc += sprintf(sql + rc, " AND %s >= :lower", qcol);
        if (qc->upper)
            rc += sprintf(sql + rc, " AND %s <= :upper", qcol);
        rc += sprintf(sql + rc, " ORDER BY %s %s", qcol, qc->by == ID ? "ASC" : "DESC");
    }
    if (qc->maxcnt)
        rc += sprintf(sql + rc, " LIMIT :maxcnt");

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return NULL;
    }

    rc = sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":channel_id"),
                            chan_id);
    if (qc->lower) {
        rc |= sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":lower"),
                                qc->lower);
    }
    if (qc->upper) {
        rc |= sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":upper"),
                                qc->upper);
    }
    if (qc->maxcnt) {
        rc |= sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":maxcnt"),
                                qc->maxcnt);
    }
    if (SQLITE_OK != rc) {
        vlogE(TAG_DB "Binding parameter channel_id failed");
        sqlite3_finalize(stmt);
        return NULL;
    }

    it = it_create(stmt, row2post);
    if (!it) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    char* exsql = sqlite3_expanded_sql(stmt);
    vlogD(TAG_DB "get posts sql: %s", exsql);
    sqlite3_free(exsql);

    return it;
}

static
void *row2postlac(sqlite3_stmt *stmt)
{
    PostInfo *pi = (PostInfo *)rc_zalloc(sizeof(PostInfo), NULL);
    if (!pi) {
        vlogE(TAG_DB "OOM");
        return NULL;
    }

    pi->chan_id = sqlite3_column_int64(stmt, 0);
    pi->post_id = sqlite3_column_int64(stmt, 1);
    pi->cmts    = sqlite3_column_int64(stmt, 2);
    pi->likes   = sqlite3_column_int64(stmt, 3);

    return pi;
}

DBObjIt *db_iter_posts_lac(uint64_t chan_id, const QryCriteria *qc)
{
    sqlite3_stmt *stmt;
    const char *qcol;
    char sql[1024] = {0};
    DBObjIt *it;
    int rc;

    rc = sprintf(sql,
                 "SELECT channel_id, post_id, next_comment_id - 1 AS comments, likes"
                 "  FROM posts "
                 "  WHERE channel_id = :channel_id");
    if (qc->by) {
        qcol = query_column(POST, (QryFld)qc->by);
        if (qc->lower)
            rc += sprintf(sql + rc, " AND %s >= :lower", qcol);
        if (qc->upper)
            rc += sprintf(sql + rc, " AND %s <= :upper", qcol);
        rc += sprintf(sql + rc, " ORDER BY %s %s", qcol, qc->by == ID ? "ASC" : "DESC");
    }
    if (qc->maxcnt)
        rc += sprintf(sql + rc, " LIMIT :maxcnt");

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return NULL;
    }

    rc = sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":channel_id"),
                            chan_id);
    if (qc->lower) {
        rc |= sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":lower"),
                                qc->lower);
    }
    if (qc->upper) {
        rc |= sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":upper"),
                                qc->upper);
    }
    if (qc->maxcnt) {
        rc |= sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":maxcnt"),
                                qc->maxcnt);
    }
    if (SQLITE_OK != rc) {
        vlogE(TAG_DB "Binding parameter channel_id failed");
        sqlite3_finalize(stmt);
        return NULL;
    }

    it = it_create(stmt, row2postlac);
    if (!it) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    return it;
}

static
void *row2likedpost(sqlite3_stmt *stmt)
{
    size_t len = sqlite3_column_int64(stmt, 3);
    PostInfo *pi = (PostInfo *)rc_zalloc(sizeof(PostInfo) + len, NULL);
    void *buf;

    if (!pi) {
        vlogE(TAG_DB "OOM");
        return NULL;
    }

    pi->chan_id    = sqlite3_column_int64(stmt, 0);
    pi->post_id    = sqlite3_column_int64(stmt, 1);
    buf = pi + 1;
    pi->content    = memcpy(buf, sqlite3_column_blob(stmt, 2), len);
    pi->len        = len;
    pi->cmts       = sqlite3_column_int64(stmt, 4);
    pi->likes      = sqlite3_column_int64(stmt, 5);
    pi->created_at = sqlite3_column_int64(stmt, 6);

    return pi;
}

DBObjIt *db_iter_liked_posts(uint64_t uid, const QryCriteria *qc)
{
    sqlite3_stmt *stmt;
    const char *qcol;
    char sql[1024] = {0};
    DBObjIt *it;
    int rc;

    rc = sprintf(sql,
                 "SELECT channel_id, post_id, content, length(content), "
                 "       next_comment_id - 1 AS comments, likes, created_at "
                 "  FROM (SELECT channel_id, post_id "
                 "          FROM likes "
                 "          WHERE user_id = :uid AND comment_id = 0) JOIN "
                 "       posts USING (channel_id, post_id)"
                 "  WHERE status = :avail");
    if (qc->by) {
        qcol = query_column(POST, (QryFld)qc->by);
        if (qc->lower)
            rc += sprintf(sql + rc, " AND %s >= :lower", qcol);
        if (qc->upper)
            rc += sprintf(sql + rc, " AND %s <= :upper", qcol);
        rc += sprintf(sql + rc, " ORDER BY %s %s", qcol, qc->by == ID ? "ASC" : "DESC");
    }
    if (qc->maxcnt)
        rc += sprintf(sql + rc, " LIMIT :maxcnt");

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return NULL;
    }

    rc = sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":uid"),
                            uid);
    rc |= sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":avail"),
                            POST_AVAILABLE);
    if (qc->lower) {
        rc |= sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":lower"),
                                qc->lower);
    }
    if (qc->upper) {
        rc |= sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":upper"),
                                qc->upper);
    }
    if (qc->maxcnt) {
        rc |= sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":maxcnt"),
                                qc->maxcnt);
    }
    if (SQLITE_OK != rc) {
        vlogE(TAG_DB "Binding parameter avail failed");
        sqlite3_finalize(stmt);
        return NULL;
    }

    it = it_create(stmt, row2likedpost);
    if (!it) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    return it;
}

static
void *row2cmt(sqlite3_stmt *stmt)
{
    CmtStat stat = (CmtStat)sqlite3_column_int64(stmt, 3);
    size_t content_len = stat == CMT_AVAILABLE ? sqlite3_column_int64(stmt, 8) : 0;
    const char *name = (const char *)sqlite3_column_text(stmt, 5);
    const char *did = (const char *)sqlite3_column_text(stmt, 6);
    CmtInfo *ci = (CmtInfo *)rc_zalloc(sizeof(CmtInfo) + content_len +
                            strlen(name) + strlen(did) + 2, NULL);
    void *buf;

    if (!ci) {
        vlogE(TAG_DB "OOM");
        return NULL;
    }

    buf = ci + 1;

    ci->chan_id      = sqlite3_column_int64(stmt, 0);
    ci->post_id      = sqlite3_column_int64(stmt, 1);
    ci->cmt_id       = sqlite3_column_int64(stmt, 2);
    ci->stat         = stat;
    ci->reply_to_cmt = sqlite3_column_int64(stmt, 4);
    ci->user.name    = strcpy((char *)buf, name);
    buf += strlen(name) + 1;
    ci->user.did     = strcpy((char *)buf, did);
    if (stat == CMT_AVAILABLE) {
        buf += strlen(did) + 1;
        ci->content  = memcpy(buf, sqlite3_column_blob(stmt, 7), content_len);
        ci->len      = content_len;
    }
    ci->likes        = sqlite3_column_int64(stmt, 9);
    ci->created_at   = sqlite3_column_int64(stmt, 10);
    ci->upd_at       = sqlite3_column_int64(stmt, 11);

    return ci;
}

static
void *row2reportedcmt(sqlite3_stmt *stmt)
{
    const char *name = (const char *)sqlite3_column_text(stmt, 3);
    const char *did = (const char *)sqlite3_column_text(stmt, 4);
    const char *reasons = (const char *)sqlite3_column_text(stmt, 5);
    ReportedCmtInfo *rci = (ReportedCmtInfo *)rc_zalloc(sizeof(CmtInfo) +
                            strlen(name) + strlen(did) + strlen(reasons) + 3, NULL);
    void *buf;

    if (!rci) {
        vlogE(TAG_DB "OOM");
        return NULL;
    }

    buf = rci + 1;

    rci->chan_id      = sqlite3_column_int64(stmt, 0);
    rci->post_id      = sqlite3_column_int64(stmt, 1);
    rci->cmt_id       = sqlite3_column_int64(stmt, 2);
    rci->reporter.name    = strcpy((char *)buf, name);
    buf += strlen(name) + 1;
    rci->reporter.did     = strcpy((char *)buf, did);
    buf += strlen(did) + 1;
    rci->reasons     = strcpy((char *)buf, reasons);
    rci->created_at   = sqlite3_column_int64(stmt, 6);

    return rci;
}

DBObjIt *db_iter_cmts(uint64_t chan_id, uint64_t post_id, const QryCriteria *qc)
{
    sqlite3_stmt *stmt;
    const char *qcol;
    char sql[1024] = {0};
    DBObjIt *it;
    int rc;

    rc = sprintf(sql,
                 "SELECT channel_id, post_id, comment_id, status, refcomment_id, "
                 "       name, did, content, length(content), likes, created_at, updated_at "
                 "  FROM comments JOIN users USING (user_id) "
                 "  WHERE channel_id = :channel_id AND post_id = :post_id");
    if (qc->by) {
        qcol = query_column(COMMENT, (QryFld)qc->by);
        if (qc->lower)
            rc += sprintf(sql + rc, " AND %s >= :lower", qcol);
        if (qc->upper)
            rc += sprintf(sql + rc, " AND %s <= :upper", qcol);
        rc += sprintf(sql + rc, " ORDER BY %s %s", qcol, qc->by == ID ? "ASC" : "DESC");
    }
    if (qc->maxcnt)
        rc += sprintf(sql + rc, " LIMIT :maxcnt");

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return NULL;
    }

    rc = sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":channel_id"),
                            chan_id);
    rc |= sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":post_id"),
                            post_id);
    if (qc->lower) {
        rc |= sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":lower"),
                                qc->lower);
    }
    if (qc->upper) {
        rc |= sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":upper"),
                                qc->upper);
    }
    if (qc->maxcnt) {
        rc |= sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":maxcnt"),
                                qc->maxcnt);
    }
    if (SQLITE_OK != rc) {
        vlogE(TAG_DB "Binding parameter post_id failed");
        sqlite3_finalize(stmt);
        return NULL;
    }

    it = it_create(stmt, row2cmt);
    if (!it) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    return it;
}

static
void *row2cmtlikes(sqlite3_stmt *stmt)
{
    CmtInfo *ci = (CmtInfo *)rc_zalloc(sizeof(CmtInfo), NULL);
    if (!ci) {
        vlogE(TAG_DB "OOM");
        return NULL;
    }

    ci->chan_id = sqlite3_column_int64(stmt, 0);
    ci->post_id = sqlite3_column_int64(stmt, 1);
    ci->cmt_id  = sqlite3_column_int64(stmt, 2);
    ci->likes   = sqlite3_column_int64(stmt, 3);

    return ci;
}

DBObjIt *db_iter_cmts_likes(uint64_t chan_id, uint64_t post_id, const QryCriteria *qc)
{
    sqlite3_stmt *stmt;
    const char *qcol;
    char sql[1024] = {0};
    DBObjIt *it;
    int rc;

    rc = sprintf(sql,
                 "SELECT channel_id, post_id, comment_id, likes"
                 "  FROM comments"
                 "  WHERE channel_id = :channel_id AND post_id = :post_id");
    if (qc->by) {
        qcol = query_column(COMMENT, (QryFld)qc->by);
        if (qc->lower)
            rc += sprintf(sql + rc, " AND %s >= :lower", qcol);
        if (qc->upper)
            rc += sprintf(sql + rc, " AND %s <= :upper", qcol);
        rc += sprintf(sql + rc, " ORDER BY %s %s", qcol, qc->by == ID ? "ASC" : "DESC");
    }
    if (qc->maxcnt)
        rc += sprintf(sql + rc, " LIMIT :maxcnt");

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return NULL;
    }

    rc = sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":channel_id"),
                            chan_id);

    rc |= sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":post_id"),
                            post_id);
    if (qc->lower) {
        rc |= sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":lower"),
                                qc->lower);
    }
    if (qc->upper) {
        rc |= sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":upper"),
                                qc->upper);
    }
    if (qc->maxcnt) {
        rc |= sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":maxcnt"),
                                qc->maxcnt);
    }
    if (SQLITE_OK != rc) {
        vlogE(TAG_DB "Binding parameter failed");
        sqlite3_finalize(stmt);
        return NULL;
    }

    it = it_create(stmt, row2cmtlikes);
    if (!it) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    return it;
}

int db_iter_nxt(DBObjIt *it, void **obj)
{
    int rc;

    rc = sqlite3_step(it->stmt);
    if (rc != SQLITE_ROW) {
        if (rc != SQLITE_DONE)
            vlogE(TAG_DB "sqlite3_step() failed");
        *obj = NULL;
        return rc == SQLITE_DONE ? 1 : -1;
    }

    *obj = it->cb(it->stmt);
    return *obj ? 0 : -1;
}

int db_is_suber(uint64_t uid, uint64_t chan_id)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "SELECT EXISTS("
          "  SELECT * "
          "  FROM subscriptions "
          "  WHERE user_id = :uid AND channel_id = :channel_id"
          ")";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_bind_int64(stmt,
                            sqlite3_bind_parameter_index(stmt, ":uid"),
                            uid);
    rc |= sqlite3_bind_int64(stmt,
                            sqlite3_bind_parameter_index(stmt, ":channel_id"),
                            chan_id);
    if (SQLITE_OK != rc) {
        vlogE(TAG_DB "Binding parameter failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    if (SQLITE_ROW != sqlite3_step(stmt)) {
        vlogE(TAG_DB "Executing SELECT failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    rc = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    return rc;
}

void db_deinit()
{
    // sqlite3_close(db);
    // sqlite3_shutdown();
}

static
void dbuinfo_dtor(void *obj)
{
    DBUserInfo *ui = (DBUserInfo *)obj;

    if (ui->stmt)
        sqlite3_finalize(ui->stmt);
}

int db_get_owner(UserInfo **ui)
{
    sqlite3_stmt *stmt;
    const char *sql;
    DBUserInfo *tmp;
    int rc;

    sql = "SELECT did, name, email FROM users WHERE user_id = :owner_user_id";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_bind_int64(stmt,
                            sqlite3_bind_parameter_index(stmt, ":owner_user_id"),
                            OWNER_USER_ID);
    if (SQLITE_OK != rc) {
        vlogE(TAG_DB "Binding parameter owner_uiser_id failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    rc = sqlite3_step(stmt);
    if (SQLITE_DONE  == rc) {
        sqlite3_finalize(stmt);
        *ui = NULL;
        return 0;
    }

    if (SQLITE_ROW != rc) {
        vlogE(TAG_DB "Executing SELECT failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    tmp = (DBUserInfo *)rc_zalloc(sizeof(DBUserInfo), dbuinfo_dtor);
    if (!tmp) {
        vlogE(TAG_DB "OOM");
        sqlite3_finalize(stmt);
        return -1;
    }

    tmp->info.uid   = OWNER_USER_ID;
    tmp->info.did   = (char *)sqlite3_column_text(stmt, 0);
    tmp->info.name  = (char *)sqlite3_column_text(stmt, 1);
    tmp->info.email = (char *)sqlite3_column_text(stmt, 2);
    tmp->stmt       = stmt;

    *ui = &tmp->info;

    return 0;
}

int db_need_upsert_user(const char *did)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "SELECT EXISTS(SELECT * FROM users WHERE did = :did AND name != 'NA')";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_bind_text(stmt,
                           sqlite3_bind_parameter_index(stmt, ":did"),
                           did, -1, NULL);
    if (SQLITE_OK != rc) {
        vlogE(TAG_DB "Binding parameter did failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    if (SQLITE_ROW != sqlite3_step(stmt)) {
        vlogE(TAG_DB "Executing SELECT failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    rc = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    return rc ? 0 : 1;
}

int db_get_user(const char *did, UserInfo **ui)
{
    sqlite3_stmt *stmt;
    const char *sql;
    DBUserInfo *tmp;
    int rc;

    sql = "SELECT user_id, did, name, email FROM users WHERE did = :did;";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_bind_text(stmt,
                           sqlite3_bind_parameter_index(stmt, ":did"),
                           did, -1, NULL);
    if (SQLITE_OK != rc) {
        vlogE(TAG_DB "Binding parameter did failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    rc = sqlite3_step(stmt);
    if (SQLITE_DONE == rc) {
        sqlite3_finalize(stmt);
        *ui = NULL;
        return 0;
    }

    if (SQLITE_ROW != rc) {
        vlogE(TAG_DB "Executing SELECT failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    tmp = (DBUserInfo *)rc_zalloc(sizeof(DBUserInfo), dbuinfo_dtor);
    if (!tmp) {
        vlogE(TAG_DB "OOM");
        sqlite3_finalize(stmt);
        return -1;
    }

    tmp->info.uid   = sqlite3_column_int64(stmt, 0);
    tmp->info.did   = (char *)sqlite3_column_text(stmt, 1);
    tmp->info.name  = (char *)sqlite3_column_text(stmt, 2);
    tmp->info.email = (char *)sqlite3_column_text(stmt, 3);
    tmp->stmt       = stmt;

    *ui = &tmp->info;

    return 0;
}

int db_get_count(const char *table_name)
{
    sqlite3_stmt *stmt;
    char sql[128] = {0};
    int rc;

    snprintf(sql, sizeof(sql), "SELECT count(*) FROM %s", table_name);

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    if (SQLITE_ROW != sqlite3_step(stmt)) {
        vlogE(TAG_DB "Executing SELECT failed");
        sqlite3_finalize(stmt);
        return -1;
    }

    rc = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    return rc;
}

int db_add_reported_cmts(uint64_t channel_id, uint64_t post_id, uint64_t comment_id,
        uint64_t reporter_id, const char *reason)
{
    sqlite3_stmt *stmt;
    const char *sql;
    int rc;

    sql = "BEGIN";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "Exectuing BEGIN failed");
        return -1;
    }

    do {
        sql = "INSERT OR REPLACE INTO reported_comments (channel_id, post_id, comment_id, reporter_id, created_at, reasons) "
              "  VALUES (:channel_id, :post_id, :comment_id, :reporter_id, :created_at, :reasons)";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":channel_id"),
                channel_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":post_id"),
                post_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":comment_id"),
                comment_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":reporter_id"),
                reporter_id);
        rc |= sqlite3_bind_int64(stmt,
                sqlite3_bind_parameter_index(stmt, ":created_at"),
                time(NULL));
        rc |= sqlite3_bind_text(stmt,
                sqlite3_bind_parameter_index(stmt, ":reasons"),
                reason, -1, NULL);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter failed");
            sqlite3_finalize(stmt);
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing INSERT failed");
            break;
        }

        sql = "END";

        if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
            vlogE(TAG_DB "sqlite3_prepare_v2() failed");
            break;
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (SQLITE_DONE != rc) {
            vlogE(TAG_DB "Executing END failed");
            break;
        }

        return 0;
    } while(0);

    sql = "ROLLBACK";

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return -1;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (SQLITE_DONE != rc) {
        vlogE(TAG_DB "ROLLBACK failed");
    }

    return -1;
}

DBObjIt *db_iter_reported_cmts(const QryCriteria *qc)
{
    sqlite3_stmt *stmt;
    const char *qcol;
    char sql[1024] = {0};
    DBObjIt *it;
    int rc;

    rc = sprintf(sql,
                 "SELECT channel_id, post_id, comment_id, name, did, reasons, created_at"
                 " FROM reported_comments"
                 " JOIN users ON reported_comments.reporter_id = users.user_id"
                 " WHERE true");

    if (qc->by) {
        qcol = query_column(COMMENT, (QryFld)qc->by);
        if (qc->lower)
            rc += sprintf(sql + rc, " AND %s >= :lower", qcol);
        if (qc->upper)
            rc += sprintf(sql + rc, " AND %s <= :upper", qcol);
        rc += sprintf(sql + rc, " ORDER BY %s %s", qcol, qc->by == ID ? "ASC" : "DESC");
    }
    if (qc->maxcnt)
        rc += sprintf(sql + rc, " LIMIT :maxcnt");
    vlogD(TAG_DB "db_iter_reported_cmts() origin sql: %s", sql);

    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
        vlogE(TAG_DB "sqlite3_prepare_v2() failed");
        return NULL;
    }

    if (qc->lower) {
        rc = sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":lower"),
                                qc->lower);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter lower failed");
            sqlite3_finalize(stmt);
            return NULL;
        }
    }

    if (qc->upper) {
        rc = sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":upper"),
                                qc->upper);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter upper failed");
            sqlite3_finalize(stmt);
            return NULL;
        }
    }

    if (qc->maxcnt) {
        rc = sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":maxcnt"),
                                qc->maxcnt);
        if (SQLITE_OK != rc) {
            vlogE(TAG_DB "Binding parameter maxcnt failed");
            sqlite3_finalize(stmt);
            return NULL;
        }
    }

    // char* expanded_sql = sqlite3_expanded_sql(stmt);
    // vlogI(TAG_DB "db_iter_reported_cmts() expanded sql: %s", expanded_sql);
    // sqlite3_free(expanded_sql);

    it = it_create(stmt, row2reportedcmt);
    if (!it) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    return it;
}
