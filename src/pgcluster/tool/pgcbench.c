/*
 * pgbench: a simple benchmark program for PGCluster
 * This program was written based on pgbench by Tatsuo Ishii.
 *
 * Portions Copyright (c) 2003-2008, Atsushi Mitani
 * Portions Copyright (c) 2000-2008, Tatsuo Ishii
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that copyright notice and this permission
 * notice appear in supporting documentation, and that the name of the
 * author not be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior
 * permission. The author makes no representations about the
 * suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 */
#include "postgres_fe.h"

#include "libpq-fe.h"

#include <errno.h>

#ifdef WIN32
#include "win32.h"
#else
#include <sys/time.h>
#include <unistd.h>

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

/* for getrlimit */
#include <sys/resource.h>
#endif   /* ! WIN32 */

#include <sys/types.h>
#include <sys/wait.h>

#include <ctype.h>
#include <search.h>

extern char *optarg;
extern int	optind;

#ifdef WIN32
#undef select
#endif


/********************************************************************
 * some configurable parameters */

#define MAXCLIENTS 4096			/* max number of clients allowed */

int			nclients = 1;		/* default number of simulated clients */
int			nxacts = 10;		/* default number of transactions per
								 * clients */

/*
 * scaling factor. for example, tps = 10 will make 1000000 tuples of
 * accounts table.
 */
int			tps = 1;

/*
 * end of configurable parameters
 *********************************************************************/

#define nbranches	1
#define ntellers	10
#define naccounts	100000

#define SELECT_ONLY	(1)
#define	INSERT_ONLY	(2)
#define	UPDATE_ONLY	(3)
#define	WITH_TRANSACTION	(4)
#define TPC_B_LIKE	(5)
#define CUSTOM_QUERY	(6)

#define SQL_COMMAND		1
#define META_COMMAND	2

FILE	   *LOGFILE = NULL;

bool		use_log = false;			/* log transaction latencies to a file */

int			remains;			/* number of remaining clients */

int			is_connect;			/* establish connection  for each
								 * transaction */

char	   *pghost = "";
char	   *pgport = NULL;
char	   *pgoptions = NULL;
char	   *pgtty = NULL;
char	   *login = NULL;
char	   *pwd = NULL;
char	   *dbName;

typedef struct
{
	char	   *name;
	char	   *value;
}	Variable;

typedef struct
{
	PGconn	   *con;			/* connection handle to DB */
	int			id;				/* client No. */
	int			state;			/* state No. */
	int			cnt;			/* xacts count */
	int			ecnt;			/* error count */
	int         maxAct;
	int			listen;			/* 0 indicates that an async query has
								 * been sent */
	int			aid;			/* account id for this transaction */
	int			bid;			/* branch id for this transaction */
	int			tid;			/* teller id for this transaction */
	int			delta;
	int			abalance;
	void	   *variables;
	struct timeval txn_begin;	/* used for measuring latencies */
}	CState;

typedef struct
{
	int			type;
	int			argc;
	char	  **argv;
}	Command;

Command	  **commands = NULL;

static void
usage(void)
{
	fprintf(stderr, "usage: pgcbench [-h hostname][-p port][-c nclients][-t ntransactions][-s scaling_factor][-I(insert only)][-U(update only)][-S(select only)][-f filename][-u login][-P password][-d(debug)][dbname]\n");
	fprintf(stderr, "(initialize mode): pgcbench -i [-h hostname][-p port][-s scaling_factor][-u login][-P password][-d(debug)][dbname]\n");
}

/* random number generator */
static int
getrand(int min, int max )
{

	return (min + (int) (max * 1.0 * rand() / (RAND_MAX + 1.0)));
}

/* set up a connection to the backend */
static PGconn *
doConnect(void)
{
	PGconn	   *con;
	PGresult   *res;

	con = PQsetdbLogin(pghost, pgport, pgoptions, pgtty, dbName,
					   login, pwd);
	if (con == NULL)
	{
		fprintf(stderr, "Connection to database '%s' failed.\n", dbName);
		fprintf(stderr, "Memory allocatin problem?\n");
		return (NULL);
	}

	if (PQstatus(con) == CONNECTION_BAD)
	{
		fprintf(stderr, "Connection to database '%s' failed.\n", dbName);

		if (PQerrorMessage(con))
			fprintf(stderr, "%s", PQerrorMessage(con));
		else
			fprintf(stderr, "No explanation from the backend\n");

		return (NULL);
	}

	res = PQexec(con, "SET search_path = public");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "%s", PQerrorMessage(con));
		exit(1);
	}
	PQclear(res);

	return (con);
}

/* throw away response from backend */
static void
discard_response(CState * state)
{
	PGresult   *res;

	do
	{
		res = PQgetResult(state->con);
		if (res)
			PQclear(res);
	} while (res);
}

/* check to see if the SQL result was good */
static int
check(CState * st, PGresult *res, int good)
{
	if (res && PQresultStatus(res) != good)
	{
		fprintf(stderr, "aborted in state %d: %s",  st->state, PQerrorMessage(st->con));
		PQfinish(st->con);
		st->con = NULL;
		return (-1);
	}
	return (0);					/* OK */
}

static int
compareVariables(const void *v1, const void *v2)
{
	return strcmp(((Variable *)v1)->name, ((Variable *)v2)->name);
}

static char *
getVariable(CState * st, char *name)
{
	Variable		key = { name }, *var;

	var = tfind(&key, &st->variables, compareVariables);
	if (var != NULL)
		return (*(Variable **)var)->value;
	else
		return NULL;
}

static int
putVariable(CState * st, char *name, char *value)
{
	Variable		key = { name }, *var;

	var = tfind(&key, &st->variables, compareVariables);
	if (var == NULL)
	{
		if ((var = malloc(sizeof(Variable))) == NULL)
			return false;

		var->name = NULL;
		var->value = NULL;

		if ((var->name = strdup(name)) == NULL
			|| (var->value = strdup(value)) == NULL
			|| tsearch(var, &st->variables, compareVariables) == NULL)
		{
			free(var->name);
			free(var->value);
			free(var);
			return false;
		}
	}
	else
	{
		free((*(Variable **)var)->value);
		if (((*(Variable **)var)->value = strdup(value)) == NULL)
			return false;
	}

	return true;
}

static char *
assignVariables(CState * st, char *sql)
{
	int			i, j;
	char	   *p, *name, *val;
	void	   *tmp;

	i = 0;
	while ((p = strchr(&sql[i], ':')) != NULL)
	{
		i = j = p - sql;
		do
			i++;
		while (isalnum(sql[i]) != 0 || sql[i] == '_');
		if (i == j + 1)
			continue;

		name = malloc(i - j);
		if (name == NULL)
			return NULL;
		memcpy(name, &sql[j + 1], i - (j + 1));
		name[i - (j + 1)] = '\0';
		val = getVariable(st, name);
		free(name);
		if (val == NULL)
			continue;

		if (strlen(val) > i - j)
		{
			tmp = realloc(sql, strlen(sql) - (i - j) + strlen(val) + 1);
			if (tmp == NULL)
			{
				free(sql);
				return NULL;
			}
			sql = tmp;
		}

		if (strlen(val) != i - j)
			memmove(&sql[j + strlen(val)], &sql[i], strlen(&sql[i]) + 1);

		strncpy(&sql[j], val, strlen(val));

		if (strlen(val) < i - j)
		{
			tmp = realloc(sql, strlen(sql) + 1);
			if (tmp == NULL)
			{
				free(sql);
				return NULL;
			}
			sql = tmp;
		}

		i = j + strlen(val);
	}

	return sql;
}

/* process a transaction */
static void
doMix(CState * st, int debug, int ttype)
{
	char		sql[256];
	PGresult   *res;

	if (st->listen)
	{							/* are we receiver? */
		if (debug)
			fprintf(stderr, "client receiving\n");
		if (!PQconsumeInput(st->con))
		{						/* there's something wrong */
			fprintf(stderr, "Client aborted in state %d. Probably the backend died while processing.\n", st->state);
			PQfinish(st->con);
			st->con = NULL;
			return;
		}
		if (PQisBusy(st->con))
			return;				/* don't have the whole result yet */

		switch (st->state)
		{
			case 0:				/* response to "begin" */
				res = PQgetResult(st->con);
				if (ttype == WITH_TRANSACTION)
				{
					if (check(st, res, PGRES_COMMAND_OK))
						return;
				}
				else
				{
					if (check(st, res, PGRES_TUPLES_OK))
						return;
				}
				PQclear(res);
				discard_response(st);
				break;
			case 1:				/* response to "update accounts..." */
				res = PQgetResult(st->con);
				if (check(st, res, PGRES_COMMAND_OK))
					return;
				PQclear(res);
				discard_response(st);
				break;
			case 2:				/* response to "select abalance ..." */
				res = PQgetResult(st->con);
				if (check(st, res, PGRES_TUPLES_OK))
					return;
				PQclear(res);
				discard_response(st);
				break;
			case 3:				/* response to "update tellers ..." */
				res = PQgetResult(st->con);
				if (check(st, res, PGRES_COMMAND_OK))
					return;
				PQclear(res);
				discard_response(st);
				break;
			case 4:				/* response to "update branches ..." */
				res = PQgetResult(st->con);
				if (check(st, res, PGRES_COMMAND_OK))
					return;
				PQclear(res);
				discard_response(st);
				break;
			case 5:				/* response to "insert into history ..." */
				res = PQgetResult(st->con);
				if (check(st, res, PGRES_COMMAND_OK))
					return;
				PQclear(res);
				discard_response(st);
				break;
			case 6:				/* response to "end" */

				/*
				 * transaction finished: record the time it took in the
				 * log
				 */
				if (use_log)
				{
					double		diff;
					struct timeval now;

					gettimeofday(&now, NULL);
					diff = (int) (now.tv_sec - st->txn_begin.tv_sec) * 1000000.0 +
						(int) (now.tv_usec - st->txn_begin.tv_usec);

					fprintf(LOGFILE, "%d %d %.0f\n", st->id, st->cnt, diff);
				}

				res = PQgetResult(st->con);
				if (ttype == WITH_TRANSACTION)
				{
					if (check(st, res, PGRES_COMMAND_OK))
						return;
				}
				else
				{
					if (check(st, res, PGRES_TUPLES_OK))
						return;
				}
				PQclear(res);
				discard_response(st);

				if (is_connect)
				{
					PQfinish(st->con);
					st->con = NULL;
				}
				if (++st->cnt >= st->maxAct)
				{
					remains--;			/* I've done */
					if (st->con != NULL)
					{
						PQfinish(st->con);
						st->con = NULL;
					}
					return;
				}
				break;
		}

		/* increment state counter */
		st->state++;
		if (st->state > 6)
		{
			st->state = 0;
			remains--;			/* I've done */
		}
	}

	if (st->con == NULL)
	{
		if ((st->con = doConnect()) == NULL)
		{
			fprintf(stderr, "Client aborted in establishing connection.\n");
			remains--;			/* I've aborted */
			PQfinish(st->con);
			st->con = NULL;
			return;
		}
	}

	switch (st->state)
	{
		case 0:			/* about to start */
			if (ttype == WITH_TRANSACTION)
			{
				strcpy(sql, "begin");
			}
			else
			{
				st->aid = getrand(1, naccounts * tps);
				snprintf(sql, 256, "select abalance from accounts where aid = %d", st->aid);
			}
			st->aid = getrand(1, naccounts * tps);
			st->bid = getrand(1, nbranches * tps);
			st->tid = getrand(1, ntellers * tps);
			st->delta = getrand(1, 1000);
			if (use_log)
				gettimeofday(&(st->txn_begin), NULL);
			break;
		case 1:
			snprintf(sql, 256, "update accounts set abalance = abalance + %d where aid = %d\n", st->delta, st->aid);
			break;
		case 2:
			snprintf(sql, 256, "select abalance from accounts where aid = %d", st->aid);
			break;
		case 3:
			if (ttype == 0)
			{
				snprintf(sql, 256, "update tellers set tbalance = tbalance + %d where tid = %d\n",
						 st->delta, st->tid);
				break;
			}
		case 4:
			if (ttype == 0)
			{
				snprintf(sql, 256, "update branches set bbalance = bbalance + %d where bid = %d", st->delta, st->bid);
				break;
			}
		case 5:
			snprintf(sql, 256, "insert into history(tid,bid,aid,delta,mtime) values(%d,%d,%d,%d,'now')",
					 st->tid, st->bid, st->aid, st->delta);
			break;
		case 6:
			if (ttype == WITH_TRANSACTION)
			{
				strcpy(sql, "end");
			}
			else
			{
				st->aid = getrand(1, naccounts * tps);
				snprintf(sql, 256, "select abalance from accounts where aid = %d", st->aid);
			}
			break;
	}

	if (debug)
		fprintf(stderr, "client sending %s\n", sql);

	if (PQsendQuery(st->con, sql) == 0)
	{
		if (debug)
			fprintf(stderr, "PQsendQuery(%s)failed\n", sql);
		st->ecnt++;
	}
	else
	{
		st->listen++;			/* flags that should be listened */
	}
}

/* process a select only transaction */
static void
doOne(CState * st, int debug, int ttype )
{
	char		sql[256];
	PGresult   *res;

	if (st->listen)
	{							/* are we receiver? */
		if (debug)
			fprintf(stderr, "client receiving\n");
		if (!PQconsumeInput(st->con))
		{						/* there's something wrong */
			fprintf(stderr, "Client aborted in state %d. Probably the backend died while processing.\n", st->state);
			remains--;			/* I've aborted */
			PQfinish(st->con);
			st->con = NULL;
			return;
		}
		if (PQisBusy(st->con))
			return;				/* don't have the whole result yet */

		switch (st->state)
		{
			case 0:				/* response to "select abalance ..." */
				res = PQgetResult(st->con);
				if (ttype == SELECT_ONLY)
				{
					if (check(st, res, PGRES_TUPLES_OK))
						return;
				}
				else
				{
					if (check(st, res, PGRES_COMMAND_OK))
						return;
				}
				PQclear(res);
				discard_response(st);

				if (is_connect)
				{
					PQfinish(st->con);
					st->con = NULL;
				}

				if (++st->cnt >= st->maxAct)
				{
					remains--;			/* I've done */
					if (st->con != NULL)
					{
						PQfinish(st->con);
						st->con = NULL;
					}
					return;
				}
				break;
		}

		/* increment state counter */
		st->state++;
		if (st->state > 0)
		{
			st->state = 0;
			remains--;	/* I've done */
		}
	}

	if (st->con == NULL)
	{
		if ((st->con = doConnect()) == NULL)
		{
			fprintf(stderr, "Client aborted in establishing connection.\n");
			PQfinish(st->con);
			st->con = NULL;
			return;
		}
	}

	switch (st->state)
	{
		case 0:
			st->aid = getrand(1, naccounts * tps);
			st->bid = getrand(1, nbranches * tps);
			st->tid = getrand(1, ntellers * tps);
			st->delta = getrand(1, 1000);
			if ( ttype == SELECT_ONLY)
			{
				snprintf(sql, 256, "select abalance from accounts where aid = %d", st->aid);
			}
			if ( ttype == UPDATE_ONLY)
			{
				snprintf(sql, 256, "update accounts set abalance = abalance + %d where aid = %d\n", st->delta, st->aid);
			}
			if ( ttype == INSERT_ONLY)
			{
				snprintf(sql, 256, "insert into history(tid,bid,aid,delta,mtime) values(%d,%d,%d,%d,'now')",
						 st->tid, st->bid, st->aid, st->delta);
			}
			break;
	}

	if (debug)
		fprintf(stderr, "client sending %s\n", sql);

	if (PQsendQuery(st->con, sql) == 0)
	{
		if (debug)
			fprintf(stderr, "PQsendQuery(%s)failed\n", sql);
		st->ecnt++;
	}
	else
	{
		st->listen++;			/* flags that should be listened */
	}
}

static void
doCustom(CState * st, int debug, int ttype )
{
	PGresult   *res;

	if (st->listen)
	{							/* are we receiver? */
		if (commands[st->state]->type == SQL_COMMAND)
		{
			if (debug)
				fprintf(stderr, "client receiving\n");
			if (!PQconsumeInput(st->con))
			{						/* there's something wrong */
				fprintf(stderr, "Client aborted in state %d. Probably the backend died while processing.\n", st->state);
				PQfinish(st->con);
				st->con = NULL;
				return;
			}
			if (PQisBusy(st->con))
				return;				/* don't have the whole result yet */
		}

		/*
		 * transaction finished: record the time it took in the
		 * log
		 */
		if (use_log && commands[st->state + 1] == NULL)
		{
			double		diff;
			struct timeval now;

			gettimeofday(&now, NULL);
			diff = (int) (now.tv_sec - st->txn_begin.tv_sec) * 1000000.0 +
				(int) (now.tv_usec - st->txn_begin.tv_usec);

			fprintf(LOGFILE, "%d %d %.0f\n", st->id, st->cnt, diff);
		}

		if (commands[st->state]->type == SQL_COMMAND)
		{
			res = PQgetResult(st->con);
			if (strncasecmp(commands[st->state]->argv[0], "select", 6) != 0)
			{
				if (check(st, res, PGRES_COMMAND_OK))
					return;
			}
			else
			{
				if (check(st, res, PGRES_TUPLES_OK))
					return;
			}
			PQclear(res);
			discard_response(st);
		}

		if (commands[st->state + 1] == NULL)
		{
			if (is_connect)
			{
				PQfinish(st->con);
				st->con = NULL;
			}
			if (++st->cnt >= st->maxAct)
			{
				remains--;			/* I've done */
				if (st->con != NULL)
				{
					PQfinish(st->con);
					st->con = NULL;
				}
				return;
			}
		}

		/* increment state counter */
		st->state++;
		if (commands[st->state] == NULL)
		{
			st->state = 0;
			remains--;			/* I've done */
		}
	}

	if (st->con == NULL)
	{
		if ((st->con = doConnect()) == NULL)
		{
			fprintf(stderr, "Client aborted in establishing connection.\n");
			remains--;			/* I've aborted */
			PQfinish(st->con);
			st->con = NULL;
			return;
		}
	}

	if (use_log && st->state == 0)
		gettimeofday(&(st->txn_begin), NULL);

	if (commands[st->state]->type == SQL_COMMAND)
	{
		char	   *sql;

		if ((sql = strdup(commands[st->state]->argv[0])) == NULL
			|| (sql = assignVariables(st, sql)) == NULL)
		{
			fprintf(stderr, "out of memory\n");
			st->ecnt++;
			return;
		}

		if (debug)
			fprintf(stderr, "client sending %s\n", sql);

		if (PQsendQuery(st->con, sql) == 0)
		{
			if (debug)
				fprintf(stderr, "PQsendQuery(%s)failed\n", sql);
			st->ecnt++;
		}
		else
		{
			st->listen++;			/* flags that should be listened */
		}

		free(sql);
	}
	else if (commands[st->state]->type == META_COMMAND)
	{
		int			argc = commands[st->state]->argc, i;
		char	  **argv = commands[st->state]->argv;

		if (debug)
		{
			fprintf(stderr, "client executing \\%s", argv[0]);
			for (i = 1; i < argc; i++)
				fprintf(stderr, " %s", argv[i]);
			fprintf(stderr, "\n");
		}

		if (strcasecmp(argv[0], "setrandom") == 0)
		{
			char	   *val;

			if ((val = malloc(strlen(argv[3]) + 1)) == NULL)
			{
				fprintf(stderr, "%s: out of memory\n", argv[0]);
				st->ecnt++;
				return;
			}

			sprintf(val, "%d", getrand(atoi(argv[2]), atoi(argv[3])));

			if (putVariable(st, argv[1], val) == false)
			{
				fprintf(stderr, "%s: out of memory\n", argv[0]);
				free(val);
				st->ecnt++;
				return;
			}

			free(val);
			st->listen++;
		}
	}
}

/* discard connections */
static void
disconnect_all(CState * state)
{
	if (state->con)
		PQfinish(state->con);
}

/* create tables and setup data */
static void
init(void)
{
	PGconn	   *con;
	PGresult   *res;
	static char *DDLs[] = {
		"drop table branches",
		"create table branches(bid int not null,bbalance int,filler char(88))",
		"drop table tellers",
		"create table tellers(tid int not null,bid int,tbalance int,filler char(84))",
		"drop table accounts",
		"create table accounts(aid int not null,bid int,abalance int,filler char(84))",
		"drop table history",
	"create table history(tid int,bid int,aid int,delta int,mtime timestamp,filler char(22))"};
	static char *DDLAFTERs[] = {
		"alter table branches add primary key (bid)",
		"alter table tellers add primary key (tid)",
	"alter table accounts add primary key (aid)"};


	char		sql[256];

	int			i;

	if ((con = doConnect()) == NULL)
		exit(1);

	for (i = 0; i < (sizeof(DDLs) / sizeof(char *)); i++)
	{
		res = PQexec(con, DDLs[i]);
		if (strncmp(DDLs[i], "drop", 4) && PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "%s", PQerrorMessage(con));
			exit(1);
		}
		PQclear(res);
	}

	res = PQexec(con, "begin");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "%s", PQerrorMessage(con));
		exit(1);
	}
	PQclear(res);

	for (i = 0; i < nbranches * tps; i++)
	{
		snprintf(sql, 256, "insert into branches(bid,bbalance) values(%d,0)", i + 1);
		res = PQexec(con, sql);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "%s", PQerrorMessage(con));
			exit(1);
		}
		PQclear(res);
	}

	for (i = 0; i < ntellers * tps; i++)
	{
		snprintf(sql, 256, "insert into tellers(tid,bid,tbalance) values (%d,%d,0)"
				 ,i + 1, i / ntellers + 1);
		res = PQexec(con, sql);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "%s", PQerrorMessage(con));
			exit(1);
		}
		PQclear(res);
	}

	res = PQexec(con, "end");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "%s", PQerrorMessage(con));
		exit(1);
	}
	PQclear(res);

	/*
	 * occupy accounts table with some data
	 */
	fprintf(stderr, "creating tables...\n");
	for (i = 0; i < naccounts * tps; i++)
	{
		int			j = i + 1;

		if (j % 10000 == 1)
		{
			res = PQexec(con, "copy accounts from stdin");
			if (PQresultStatus(res) != PGRES_COPY_IN)
			{
				fprintf(stderr, "%s", PQerrorMessage(con));
				exit(1);
			}
			PQclear(res);
		}

		snprintf(sql, 256, "%d\t%d\t%d\t\n", j, i / naccounts + 1, 0);
		if (PQputline(con, sql))
		{
			fprintf(stderr, "PQputline failed\n");
			exit(1);
		}

		if (j % 10000 == 0)
		{
			/*
			 * every 10000 tuples, we commit the copy command. this should
			 * avoid generating too much WAL logs
			 */
			fprintf(stderr, "%d tuples done.\n", j);
			if (PQputline(con, "\\.\n"))
			{
				fprintf(stderr, "very last PQputline failed\n");
				exit(1);
			}

			if (PQendcopy(con))
			{
				fprintf(stderr, "PQendcopy failed\n");
				exit(1);
			}

#ifdef NOT_USED

			/*
			 * do a checkpoint to purge the old WAL logs
			 */
			res = PQexec(con, "checkpoint");
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				fprintf(stderr, "%s", PQerrorMessage(con));
				exit(1);
			}
			PQclear(res);
#endif   /* NOT_USED */
		}
	}
	fprintf(stderr, "set primary key...\n");
	for (i = 0; i < (sizeof(DDLAFTERs) / sizeof(char *)); i++)
	{
		res = PQexec(con, DDLAFTERs[i]);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "%s", PQerrorMessage(con));
			exit(1);
		}
		PQclear(res);
	}

	/* vacuum */
	fprintf(stderr, "vacuum...");
	res = PQexec(con, "vacuum analyze");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "%s", PQerrorMessage(con));
		exit(1);
	}
	PQclear(res);
	fprintf(stderr, "done.\n");

	PQfinish(con);
}

static int
process_file(char *filename)
{
	const char	delim[] = " \f\n\r\t\v";

	FILE	   *fd;
	int			lineno, i, j;
	char		buf[BUFSIZ], *p, *tok;
	void	   *tmp;

	if (strcmp(filename, "-") == 0)
		fd = stdin;
	else if ((fd = fopen(filename, "r")) == NULL)
	{
		fprintf(stderr, "%s: %s\n", strerror(errno), filename);
		return false;
	}

	fprintf(stderr, "processing file...\n");

	lineno = 1;
	i = 0;
	while (fgets(buf, sizeof(buf), fd) != NULL)
	{
		if ((p = strchr(buf, '\n')) != NULL)
			*p = '\0';
		p = buf;
		while (isspace(*p))
			p++;
		if (*p == '\0' || strncmp(p, "--", 2) == 0)
		{
			lineno++;
			continue;
		}

		if ((tmp = realloc(commands, sizeof(Command *) * (i + 1))) == NULL)
		{
			i--;
			goto error;
		}
		commands = tmp;

		if ((commands[i] = malloc(sizeof(Command))) == NULL)
			goto error;

		commands[i]->argv = NULL;
		commands[i]->argc = 0;

		if (*p == '\\')
		{
			commands[i]->type = META_COMMAND;

			j = 0;
			tok = strtok(++p, delim);
			while (tok != NULL)
			{
				tmp = realloc(commands[i]->argv, sizeof(char *) * (j + 1));
				if (tmp == NULL)
					goto error;
				commands[i]->argv = tmp;

				if ((commands[i]->argv[j] = strdup(tok)) == NULL)
					goto error;

				commands[i]->argc++;

				j++;
				tok = strtok(NULL, delim);
			}

			if (strcasecmp(commands[i]->argv[0], "setrandom") == 0)
			{
				int			min, max;

				if (commands[i]->argc < 4)
				{
					fprintf(stderr, "%s: %d: \\%s: missing argument\n", filename, lineno, commands[i]->argv[0]);
					goto error;
				}

				for (j = 4; j < commands[i]->argc; j++)
					fprintf(stderr, "%s: %d: \\%s: extra argument \"%s\" ignored\n", filename, lineno, commands[i]->argv[0], commands[i]->argv[j]);

				if ((min = atoi(commands[i]->argv[2])) < 0)
				{
					fprintf(stderr, "%s: %d: \\%s: invalid minimum number %s\n", filename, lineno, commands[i]->argv[0], commands[i]->argv[2]);
					goto error;
				}

				if ((max = atoi(commands[i]->argv[3])) < min || max > RAND_MAX)
				{
					fprintf(stderr, "%s: %d: \\%s: invalid maximum number %s\n", filename, lineno, commands[i]->argv[0], commands[i]->argv[3]);
					goto error;
				}
			}
			else
			{
				fprintf(stderr, "%s: %d: invalid command \\%s\n", filename, lineno, commands[i]->argv[0]);
				goto error;
			}
		}
		else
		{
			commands[i]->type = SQL_COMMAND;

			if ((commands[i]->argv = malloc(sizeof(char *))) == NULL)
				goto error;

			if ((commands[i]->argv[0] = strdup(p)) == NULL)
				goto error;

			commands[i]->argc++;
		}

		i++;
		lineno++;
	}
	fclose(fd);

	if ((tmp = realloc(commands, sizeof(Command *) * (i + 1))) == NULL)
		goto error;
	commands = tmp;

	commands[i] = NULL;

	return true;

error:
	if (errno == ENOMEM)
		fprintf(stderr, "%s: %d: out of memory\n", filename, lineno);

	fclose(fd);

	if (commands == NULL)
		return false;

	while (i >= 0)
	{
		if (commands[i] != NULL)
		{
			for (j = 0; j < commands[i]->argc; j++)
				free(commands[i]->argv[j]);

			free(commands[i]->argv);
			free(commands[i]);
		}

		i--;
	}
	free(commands);

	return false;
}

/* print out results */
static void
printResults(
			 int ttype, int normal_xacts,
			 struct timeval * tv1, struct timeval * tv2,
			 struct timeval * tv3)
{
	double		t1,
				t2;
	char	   *s;

	t1 = (tv3->tv_sec - tv1->tv_sec) * 1000000.0 + (tv3->tv_usec - tv1->tv_usec);
	t1 = t1 / 1000000.0 ;

	t2 = (tv3->tv_sec - tv1->tv_sec) * 1000000.0 + (tv3->tv_usec - tv1->tv_usec);
	t2 = normal_xacts * 1000000.0 / t2;

#define SELECT_ONLY	(1)
#define	INSERT_ONLY	(2)
#define	UPDATE_ONLY	(3)
#define	WITH_TRANSACTION	(4)
	switch (ttype)
	{
		case 0:
			s = "TPC-B (sort of)";
			break;
		case SELECT_ONLY :
			s = "SELECT only";
			break;
		case INSERT_ONLY :
			s = "INSERT only";
			break;
		case UPDATE_ONLY :
			s = "UPDATE only";
			break;
		case CUSTOM_QUERY :
			s = "Custom query";
			break;
		default:
			s = "Mix query";
			break;
	}


	printf("transaction type: %s\n", s);
	printf("scaling factor: %d\n", tps);
	printf("number of clients: %d\n", nclients);
	printf("number of transactions actually processed: %d\n", normal_xacts );
	printf("run time (sec) = %f \n", t1);
	printf("tps = %f (including connections establishing)\n", t2);
}

static int
doChild(int clientId, int min, int max, int debug, int ttype)
{
	CState state;		/* status of clients */

	struct timeval tv1;			/* start up time */
	fd_set		input_mask;
	int			nsocks = 0;		/* return from select(2) */
	int			sock = 0;

	gettimeofday(&tv1, NULL);
	srand((unsigned int) tv1.tv_usec + clientId );

	memset((char *)&state,0,sizeof(CState));
	/* make connections to the database */
	state.id = clientId;
	if ((state.con = doConnect()) == NULL)
		exit(1);

	state.maxAct = max - min + 1;
	/* send start up queries in async manner */
	switch (ttype)
	{
		case WITH_TRANSACTION :
		case TPC_B_LIKE :
			doMix(&state, debug, ttype);
			break;
		case CUSTOM_QUERY :
			doCustom(&state, debug, ttype);
			break;
		default :
			doOne(&state, debug, ttype);
			break;
	}

	remains = max;
	for (;;)
	{
		if (remains < min || !state.con)
		{
			break;
		}

		FD_ZERO(&input_mask);

		if (ttype != CUSTOM_QUERY || commands[state.state]->type != META_COMMAND)
		{
			if (state.con == NULL)
			{
				if ((state.con = doConnect()) == NULL)
				{
					exit(1);
				}
			}
			sock = PQsocket(state.con);

			if (sock < 0)
			{
				fprintf(stderr, "Client %d: PQsocket failed\n", clientId);
				disconnect_all(&state);
				exit(1);
			}
			FD_SET(sock, &input_mask);

			if ((nsocks = select(sock + 1, &input_mask, (fd_set *) NULL,
							  (fd_set *) NULL, (struct timeval *) NULL)) < 0)
			{
				if (errno == EINTR)
					continue;
				/* must be something wrong */
				disconnect_all(&state);
				fprintf(stderr, "select failed: %s\n", strerror(errno));
				exit(1);
			}
			else if (nsocks == 0)
			{						/* timeout */
				fprintf(stderr, "select timeout\n");
				fprintf(stderr, "client %d:state %d cnt %d ecnt %d listen %d\n",
						clientId, state.state, state.cnt, state.ecnt, state.listen);
				exit(0);
			}
		}

		/* ok, backend returns reply */
		if (state.con && (FD_ISSET(PQsocket(state.con), &input_mask)
						  || (ttype == CUSTOM_QUERY
							  && commands[state.state]->type == META_COMMAND)))
		{
			switch (ttype)
			{
				case WITH_TRANSACTION :
				case TPC_B_LIKE :
					doMix(&state, debug, ttype);
					break;
				case CUSTOM_QUERY :
					doCustom(&state, debug, ttype);
					break;
				default :
					doOne(&state, debug, ttype);
					break;
			}
		}
	}
	disconnect_all(&state);
	return 1;
}

static int
doClient(int debug, int ttype)
{
	pid_t pid;
	int i;
	int min,max;
	int base,mo;

	base = nxacts / nclients;
	mo = nxacts % nclients;
	min = max = 0;
	for ( i = 0 ; i < nclients ; i ++)
	{
		min = max + 1;
		max += base;
		if (mo > 0)
		{
			max += 1;
			mo --;
		}
		pid = fork();
		if (pid == 0)
		{
			doChild(i, min, max, debug, ttype);
			exit(0);
		}
	}
	while ( wait(NULL) > 0)
		;
	return 1;
}

int
main(int argc, char **argv)
{
	int			c;
	int			is_init_mode = 0;		/* initialize mode? */
	int			is_no_vacuum = 0;		/* no vacuum at all before
										 * testing? */
	int			is_full_vacuum = 0;		/* do full vacuum before testing? */
	int			debug = 0;		/* debug flag */
	int			ttype = TPC_B_LIKE;		/* transaction type */
	char	   *filename = NULL;

	struct timeval tv1;			/* start up time */
	struct timeval tv2;			/* after establishing all connections to
								 * the backend */
	struct timeval tv3;			/* end time */

#if !(defined(__CYGWIN__) || defined(__MINGW32__))
	struct rlimit rlim;
#endif

	PGconn	   *con;
	PGresult   *res;
	char	   *env;

	if ((env = getenv("PGHOST")) != NULL && *env != '\0')
		pghost = env;
	if ((env = getenv("PGPORT")) != NULL && *env != '\0')
		pgport = env;
	else if ((env = getenv("PGUSER")) != NULL && *env != '\0')
		login = env;

	while ((c = getopt(argc, argv, "ih:nvp:dc:t:s:u:P:CNSlTUIf:")) != -1)
	{
		switch (c)
		{
			case 'i':
				is_init_mode++;
				break;
			case 'h':
				pghost = optarg;
				break;
			case 'n':
				is_no_vacuum++;
				break;
			case 'v':
				is_full_vacuum++;
				break;
			case 'p':
				pgport = optarg;
				break;
			case 'd':
				debug++;
				break;
			case 'S':
				ttype = SELECT_ONLY;
				break;
			case 'I':
				ttype = INSERT_ONLY;
				break;
			case 'U':
				ttype = UPDATE_ONLY;
				break;
			case 'T':
				ttype = WITH_TRANSACTION;
				break;
			case 'c':
				nclients = atoi(optarg);
				if (nclients <= 0 || nclients > MAXCLIENTS)
				{
					fprintf(stderr, "invalid number of clients: %d\n", nclients);
					exit(1);
				}
#if !(defined(__CYGWIN__) || defined(__MINGW32__))
#ifdef RLIMIT_NOFILE			/* most platform uses RLIMIT_NOFILE */
				if (getrlimit(RLIMIT_NOFILE, &rlim) == -1)
				{
#else							/* but BSD doesn't ... */
				if (getrlimit(RLIMIT_OFILE, &rlim) == -1)
				{
#endif   /* HAVE_RLIMIT_NOFILE */
					fprintf(stderr, "getrlimit failed. reason: %s\n", strerror(errno));
					exit(1);
				}
				if (rlim.rlim_cur <= (nclients + 2))
				{
					fprintf(stderr, "You need at least %d open files resource but you are only allowed to use %ld.\n", nclients + 2, (long) rlim.rlim_cur);
					fprintf(stderr, "Use limit/ulimt to increase the limit before using pgbench.\n");
					exit(1);
				}
#endif   /* #if !(defined(__CYGWIN__) || defined(__MINGW32__)) */
				break;
			case 'C':
				is_connect = 1;
				break;
			case 's':
				tps = atoi(optarg);
				if (tps <= 0)
				{
					fprintf(stderr, "invalid scaling factor: %d\n", tps);
					exit(1);
				}
				break;
			case 't':
				nxacts = atoi(optarg);
				if (nxacts <= 0)
				{
					fprintf(stderr, "invalid number of transactions: %d\n", nxacts);
					exit(1);
				}
				break;
			case 'u':
				login = optarg;
				break;
			case 'P':
				pwd = optarg;
				break;
			case 'l':
				use_log = true;
				break;
			case 'f':
				ttype = CUSTOM_QUERY;
				filename = optarg;
				break;
			default:
				usage();
				exit(1);
				break;
		}
	}

	if (argc > optind)
		dbName = argv[optind];
	else
	{
		if ((env = getenv("PGDATABASE")) != NULL && *env != '\0')
			dbName = env;
		else if (login != NULL && *login != '\0')
			dbName = login;
		else
			dbName = "";
	}

	if (is_init_mode)
	{
		init();
		exit(0);
	}

	if (use_log)
	{
		char		logpath[64];

		snprintf(logpath, 64, "pgbench_log.%d", getpid());
		LOGFILE = fopen(logpath, "w");

		if (LOGFILE == NULL)
		{
			fprintf(stderr, "Couldn't open logfile \"%s\": %s", logpath, strerror(errno));
			exit(1);
		}
	}

	if (debug)
	{
		printf("pghost: %s pgport: %s nclients: %d nxacts: %d dbName: %s\n",
			   pghost, pgport, nclients, nxacts, dbName);
	}

	/* opening connection... */
	con = doConnect();
	if (con == NULL)
		exit(1);

	if (PQstatus(con) == CONNECTION_BAD)
	{
		fprintf(stderr, "Connection to database '%s' failed.\n", dbName);
		fprintf(stderr, "%s", PQerrorMessage(con));
		exit(1);
	}

	if (ttype == CUSTOM_QUERY)
	{
		PQfinish(con);
		if (process_file(filename) == false)
			exit(1);
	}
	else
	{
		/*
		 * get the scaling factor that should be same as count(*) from
		 * branches...
		 */
		res = PQexec(con, "select count(*) from branches");
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			fprintf(stderr, "%s", PQerrorMessage(con));
			exit(1);
		}
		tps = atoi(PQgetvalue(res, 0, 0));
		if (tps < 0)
		{
			fprintf(stderr, "count(*) from branches invalid (%d)\n", tps);
			exit(1);
		}
		PQclear(res);

		if (!is_no_vacuum)
		{
			fprintf(stderr, "starting vacuum...");
			res = PQexec(con, "vacuum branches");
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				fprintf(stderr, "%s", PQerrorMessage(con));
				exit(1);
			}
			PQclear(res);

			res = PQexec(con, "vacuum tellers");
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				fprintf(stderr, "%s", PQerrorMessage(con));
				exit(1);
			}
			PQclear(res);

			res = PQexec(con, "delete from history");
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				fprintf(stderr, "%s", PQerrorMessage(con));
				exit(1);
			}
			PQclear(res);
			res = PQexec(con, "vacuum history");
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				fprintf(stderr, "%s", PQerrorMessage(con));
				exit(1);
			}
			PQclear(res);

			fprintf(stderr, "end.\n");

			if (is_full_vacuum)
			{
				fprintf(stderr, "starting full vacuum...");
				res = PQexec(con, "vacuum analyze accounts");
				if (PQresultStatus(res) != PGRES_COMMAND_OK)
				{
					fprintf(stderr, "%s", PQerrorMessage(con));
					exit(1);
				}
				PQclear(res);
				fprintf(stderr, "end.\n");
			}
		}
		PQfinish(con);
	}
	
	/* set random seed */
	gettimeofday(&tv1, NULL);
	srand((unsigned int) tv1.tv_usec);
	/* get start up time */
	gettimeofday(&tv1, NULL);
	/* time after connections set up */
	gettimeofday(&tv2, NULL);

	doClient(debug, ttype);

	/* get end time */
	gettimeofday(&tv3, NULL);
	printResults(ttype, nxacts, &tv1, &tv2, &tv3);
	if (LOGFILE)
		fclose(LOGFILE);
	return 1;
}
