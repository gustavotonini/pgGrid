/*-------------------------------------------------------------------------
 * pqformat.c
 *		Routines for formatting and parsing frontend/backend messages
 *
 * These modules copyed from src/backend/libpq/pgformat.c.
 * Original modules have some shared modules and macro,
 * then it is difficult link to replication server directory.
 * Therefore, these modules were custamized.
 * (removed shared module and macro)
 *
 * Original source code is under the following copyright
 * 
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 * Message parsing after input:
 *		pq_getmsgbyte	- get a raw byte from a message buffer
 *		pq_getmsgint	- get a binary integer from a message buffer
 *		pq_getmsgbytes	- get raw data from a message buffer
 *		pq_copymsgbytes - copy raw data from a message buffer
 *		pq_getmsgstring - get a null-terminated text string (with conversion)
 */

/* --------------------------------
 *		pq_getmsgstring - get a null-terminated text string (with conversion)
 *
 *		May return a pointer directly into the message buffer, or a pointer
 *		to a palloc'd conversion result.
 * --------------------------------
 */

#include "postgres.h"
#include <errno.h>
#include <sys/types.h>
#include <sys/param.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef HAVE_ENDIAN_H
#include <endian.h>
#endif

#include "mb/pg_wchar.h"

#include "libpq-fe.h"
#include "libpq-int.h"
#include "fe-auth.h"
#include "replicate_com.h"
#include "pgreplicate.h"

const char * pq_getmsgstring(StringInfo msg);
unsigned int pq_getmsgint(StringInfo msg, int b);
void pq_copymsgbytes(StringInfo msg, char *buf, int datalen);
const char * pq_getmsgbytes(StringInfo msg, int datalen);
int pq_getmsgbyte(StringInfo msg);

const char *
pq_getmsgstring(StringInfo msg)
{
	char	   *str;
	int			slen;

	if (msg == NULL)
	{
		return NULL;
	}
	str = &msg->data[msg->cursor];
	/*
	 * It's safe to use strlen() here because a StringInfo is guaranteed to
	 * have a trailing null byte.  But check we found a null inside the
	 * message.
	 */
	slen = strlen(str);
	if (msg->cursor + slen >= msg->len)
	{
		return NULL;
	}
	msg->cursor += slen + 1;

	return str;
}


/* --------------------------------
 *		pq_getmsgint	- get a binary integer from a message buffer
 *
 *		Values are treated as unsigned.
 * --------------------------------
 */
unsigned int
pq_getmsgint(StringInfo msg, int b)
{
	unsigned int result;
	unsigned char n8;
	uint16		n16;
	uint32		n32;

	switch (b)
	{
		case 1:
			pq_copymsgbytes(msg, (char *) &n8, 1);
			result = n8;
			break;
		case 2:
			pq_copymsgbytes(msg, (char *) &n16, 2);
			result = ntohs(n16);
			break;
		case 4:
			pq_copymsgbytes(msg, (char *) &n32, 4);
			result = ntohl(n32);
			break;
		default:
			result = 0;			/* keep compiler quiet */
			break;
	}
	return result;
}

/* --------------------------------
 *		pq_copymsgbytes - copy raw data from a message buffer
 *
 *		Same as above, except data is copied to caller's buffer.
 * --------------------------------
 */
void
pq_copymsgbytes(StringInfo msg, char *buf, int datalen)
{
	if (datalen < 0 || datalen > (msg->len - msg->cursor))
	{
		return;
	}
	memcpy(buf, &msg->data[msg->cursor], datalen);
	msg->cursor += datalen;
}


/* --------------------------------
 *		pq_getmsgbytes	- get raw data from a message buffer
 *
 *		Returns a pointer directly into the message buffer; note this
 *		may not have any particular alignment.
 * --------------------------------
 */
const char *
pq_getmsgbytes(StringInfo msg, int datalen)
{
	const char *result;

	if (datalen < 0 || datalen > (msg->len - msg->cursor))
	{
		return NULL;
	}
	result = &msg->data[msg->cursor];
	msg->cursor += datalen;
	return result;
}

/* --------------------------------
 *		pq_getmsgbyte	- get a raw byte from a message buffer
 * --------------------------------
 */
int
pq_getmsgbyte(StringInfo msg)
{
	if (msg->cursor >= msg->len)
	{
		return 0;
	}
	return (unsigned char) msg->data[msg->cursor++];
}
