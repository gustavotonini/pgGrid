/*-------------------------------------------------------------------------
 *
 * server.h
 *	  Commands for manipulating servers.
 *
 *-------------------------------------------------------------------------
 */
#ifndef SERVER_H
#define SERVER_H

#include "nodes/parsenodes.h"


extern void CreateServer(CreateServerStmt *stmt);

extern void DropServer(DropServerStmt *stmt);

#endif   /* SERVER_H */
