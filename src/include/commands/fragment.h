/*-------------------------------------------------------------------------
 *
 * fragment.h
 *	  Commands for manipulating fragment.
 *
 *-------------------------------------------------------------------------
 */
#ifndef FRAGMENT_H
#define FRAGMENT_H

#include "nodes/parsenodes.h"


extern void CreateFragment(CreateFragmentStmt *stmt, const char *queryString);

extern void DropFragment(DropFragmentStmt *stmt, const char *queryString);

extern void InsertFragmentAttribute(Oid fragmentid, Oid relid, const char *attname);

#endif   /* FRAGMENT_H */
