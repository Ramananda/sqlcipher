/*
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
**
** $Id: trigger.c,v 1.143 2009/08/10 03:57:58 shane Exp $
*/
#include "sqliteInt.h"

#ifndef SQLITE_OMIT_TRIGGER
/*
** Delete a linked list of TriggerStep structures.
*/
void sqlite3DeleteTriggerStep(sqlite3 *db, TriggerStep *pTriggerStep){
  while( pTriggerStep ){
    TriggerStep * pTmp = pTriggerStep;
    pTriggerStep = pTriggerStep->pNext;

    sqlite3ExprDelete(db, pTmp->pWhere);
    sqlite3ExprListDelete(db, pTmp->pExprList);
    sqlite3SelectDelete(db, pTmp->pSelect);
    sqlite3IdListDelete(db, pTmp->pIdList);

    sqlite3DbFree(db, pTmp);
  }
}

/*
** Given table pTab, return a list of all the triggers attached to 
** the table. The list is connected by Trigger.pNext pointers.
**
** All of the triggers on pTab that are in the same database as pTab
** are already attached to pTab->pTrigger.  But there might be additional
** triggers on pTab in the TEMP schema.  This routine prepends all
** TEMP triggers on pTab to the beginning of the pTab->pTrigger list
** and returns the combined list.
**
** To state it another way:  This routine returns a list of all triggers
** that fire off of pTab.  The list will include any TEMP triggers on
** pTab as well as the triggers lised in pTab->pTrigger.
*/
Trigger *sqlite3TriggerList(Parse *pParse, Table *pTab){
  Schema * const pTmpSchema = pParse->db->aDb[1].pSchema;
  Trigger *pList = 0;                  /* List of triggers to return */

  if( pTmpSchema!=pTab->pSchema ){
    HashElem *p;
    for(p=sqliteHashFirst(&pTmpSchema->trigHash); p; p=sqliteHashNext(p)){
      Trigger *pTrig = (Trigger *)sqliteHashData(p);
      if( pTrig->pTabSchema==pTab->pSchema
       && 0==sqlite3StrICmp(pTrig->table, pTab->zName) 
      ){
        pTrig->pNext = (pList ? pList : pTab->pTrigger);
        pList = pTrig;
      }
    }
  }

  return (pList ? pList : pTab->pTrigger);
}

/*
** This is called by the parser when it sees a CREATE TRIGGER statement
** up to the point of the BEGIN before the trigger actions.  A Trigger
** structure is generated based on the information available and stored
** in pParse->pNewTrigger.  After the trigger actions have been parsed, the
** sqlite3FinishTrigger() function is called to complete the trigger
** construction process.
*/
void sqlite3BeginTrigger(
  Parse *pParse,      /* The parse context of the CREATE TRIGGER statement */
  Token *pName1,      /* The name of the trigger */
  Token *pName2,      /* The name of the trigger */
  int tr_tm,          /* One of TK_BEFORE, TK_AFTER, TK_INSTEAD */
  int op,             /* One of TK_INSERT, TK_UPDATE, TK_DELETE */
  IdList *pColumns,   /* column list if this is an UPDATE OF trigger */
  SrcList *pTableName,/* The name of the table/view the trigger applies to */
  Expr *pWhen,        /* WHEN clause */
  int isTemp,         /* True if the TEMPORARY keyword is present */
  int noErr           /* Suppress errors if the trigger already exists */
){
  Trigger *pTrigger = 0;  /* The new trigger */
  Table *pTab;            /* Table that the trigger fires off of */
  char *zName = 0;        /* Name of the trigger */
  sqlite3 *db = pParse->db;  /* The database connection */
  int iDb;                /* The database to store the trigger in */
  Token *pName;           /* The unqualified db name */
  DbFixer sFix;           /* State vector for the DB fixer */
  int iTabDb;             /* Index of the database holding pTab */

  assert( pName1!=0 );   /* pName1->z might be NULL, but not pName1 itself */
  assert( pName2!=0 );
  assert( op==TK_INSERT || op==TK_UPDATE || op==TK_DELETE );
  assert( op>0 && op<0xff );
  if( isTemp ){
    /* If TEMP was specified, then the trigger name may not be qualified. */
    if( pName2->n>0 ){
      sqlite3ErrorMsg(pParse, "temporary trigger may not have qualified name");
      goto trigger_cleanup;
    }
    iDb = 1;
    pName = pName1;
  }else{
    /* Figure out the db that the the trigger will be created in */
    iDb = sqlite3TwoPartName(pParse, pName1, pName2, &pName);
    if( iDb<0 ){
      goto trigger_cleanup;
    }
  }

  /* If the trigger name was unqualified, and the table is a temp table,
  ** then set iDb to 1 to create the trigger in the temporary database.
  ** If sqlite3SrcListLookup() returns 0, indicating the table does not
  ** exist, the error is caught by the block below.
  */
  if( !pTableName || db->mallocFailed ){
    goto trigger_cleanup;
  }
  pTab = sqlite3SrcListLookup(pParse, pTableName);
  if( pName2->n==0 && pTab && pTab->pSchema==db->aDb[1].pSchema ){
    iDb = 1;
  }

  /* Ensure the table name matches database name and that the table exists */
  if( db->mallocFailed ) goto trigger_cleanup;
  assert( pTableName->nSrc==1 );
  if( sqlite3FixInit(&sFix, pParse, iDb, "trigger", pName) && 
      sqlite3FixSrcList(&sFix, pTableName) ){
    goto trigger_cleanup;
  }
  pTab = sqlite3SrcListLookup(pParse, pTableName);
  if( !pTab ){
    /* The table does not exist. */
    if( db->init.iDb==1 ){
      /* Ticket #3810.
      ** Normally, whenever a table is dropped, all associated triggers are
      ** dropped too.  But if a TEMP trigger is created on a non-TEMP table
      ** and the table is dropped by a different database connection, the
      ** trigger is not visible to the database connection that does the
      ** drop so the trigger cannot be dropped.  This results in an
      ** "orphaned trigger" - a trigger whose associated table is missing.
      */
      db->init.orphanTrigger = 1;
    }
    goto trigger_cleanup;
  }
  if( IsVirtual(pTab) ){
    sqlite3ErrorMsg(pParse, "cannot create triggers on virtual tables");
    goto trigger_cleanup;
  }

  /* Check that the trigger name is not reserved and that no trigger of the
  ** specified name exists */
  zName = sqlite3NameFromToken(db, pName);
  if( !zName || SQLITE_OK!=sqlite3CheckObjectName(pParse, zName) ){
    goto trigger_cleanup;
  }
  if( sqlite3HashFind(&(db->aDb[iDb].pSchema->trigHash),
                      zName, sqlite3Strlen30(zName)) ){
    if( !noErr ){
      sqlite3ErrorMsg(pParse, "trigger %T already exists", pName);
    }
    goto trigger_cleanup;
  }

  /* Do not create a trigger on a system table */
  if( sqlite3StrNICmp(pTab->zName, "sqlite_", 7)==0 ){
    sqlite3ErrorMsg(pParse, "cannot create trigger on system table");
    pParse->nErr++;
    goto trigger_cleanup;
  }

  /* INSTEAD of triggers are only for views and views only support INSTEAD
  ** of triggers.
  */
  if( pTab->pSelect && tr_tm!=TK_INSTEAD ){
    sqlite3ErrorMsg(pParse, "cannot create %s trigger on view: %S", 
        (tr_tm == TK_BEFORE)?"BEFORE":"AFTER", pTableName, 0);
    goto trigger_cleanup;
  }
  if( !pTab->pSelect && tr_tm==TK_INSTEAD ){
    sqlite3ErrorMsg(pParse, "cannot create INSTEAD OF"
        " trigger on table: %S", pTableName, 0);
    goto trigger_cleanup;
  }
  iTabDb = sqlite3SchemaToIndex(db, pTab->pSchema);

#ifndef SQLITE_OMIT_AUTHORIZATION
  {
    int code = SQLITE_CREATE_TRIGGER;
    const char *zDb = db->aDb[iTabDb].zName;
    const char *zDbTrig = isTemp ? db->aDb[1].zName : zDb;
    if( iTabDb==1 || isTemp ) code = SQLITE_CREATE_TEMP_TRIGGER;
    if( sqlite3AuthCheck(pParse, code, zName, pTab->zName, zDbTrig) ){
      goto trigger_cleanup;
    }
    if( sqlite3AuthCheck(pParse, SQLITE_INSERT, SCHEMA_TABLE(iTabDb),0,zDb)){
      goto trigger_cleanup;
    }
  }
#endif

  /* INSTEAD OF triggers can only appear on views and BEFORE triggers
  ** cannot appear on views.  So we might as well translate every
  ** INSTEAD OF trigger into a BEFORE trigger.  It simplifies code
  ** elsewhere.
  */
  if (tr_tm == TK_INSTEAD){
    tr_tm = TK_BEFORE;
  }

  /* Build the Trigger object */
  pTrigger = (Trigger*)sqlite3DbMallocZero(db, sizeof(Trigger));
  if( pTrigger==0 ) goto trigger_cleanup;
  pTrigger->name = zName;
  zName = 0;
  pTrigger->table = sqlite3DbStrDup(db, pTableName->a[0].zName);
  pTrigger->pSchema = db->aDb[iDb].pSchema;
  pTrigger->pTabSchema = pTab->pSchema;
  pTrigger->op = (u8)op;
  pTrigger->tr_tm = tr_tm==TK_BEFORE ? TRIGGER_BEFORE : TRIGGER_AFTER;
  pTrigger->pWhen = sqlite3ExprDup(db, pWhen, EXPRDUP_REDUCE);
  pTrigger->pColumns = sqlite3IdListDup(db, pColumns);
  assert( pParse->pNewTrigger==0 );
  pParse->pNewTrigger = pTrigger;

trigger_cleanup:
  sqlite3DbFree(db, zName);
  sqlite3SrcListDelete(db, pTableName);
  sqlite3IdListDelete(db, pColumns);
  sqlite3ExprDelete(db, pWhen);
  if( !pParse->pNewTrigger ){
    sqlite3DeleteTrigger(db, pTrigger);
  }else{
    assert( pParse->pNewTrigger==pTrigger );
  }
}

/*
** This routine is called after all of the trigger actions have been parsed
** in order to complete the process of building the trigger.
*/
void sqlite3FinishTrigger(
  Parse *pParse,          /* Parser context */
  TriggerStep *pStepList, /* The triggered program */
  Token *pAll             /* Token that describes the complete CREATE TRIGGER */
){
  Trigger *pTrig = pParse->pNewTrigger;    /* Trigger being finished */
  char *zName;                             /* Name of trigger */
  sqlite3 *db = pParse->db;                /* The database */
  DbFixer sFix;
  int iDb;                                 /* Database containing the trigger */
  Token nameToken;           /* Trigger name for error reporting */

  pTrig = pParse->pNewTrigger;
  pParse->pNewTrigger = 0;
  if( NEVER(pParse->nErr) || !pTrig ) goto triggerfinish_cleanup;
  zName = pTrig->name;
  iDb = sqlite3SchemaToIndex(pParse->db, pTrig->pSchema);
  pTrig->step_list = pStepList;
  while( pStepList ){
    pStepList->pTrig = pTrig;
    pStepList = pStepList->pNext;
  }
  nameToken.z = pTrig->name;
  nameToken.n = sqlite3Strlen30(nameToken.z);
  if( sqlite3FixInit(&sFix, pParse, iDb, "trigger", &nameToken) 
          && sqlite3FixTriggerStep(&sFix, pTrig->step_list) ){
    goto triggerfinish_cleanup;
  }

  /* if we are not initializing, and this trigger is not on a TEMP table, 
  ** build the sqlite_master entry
  */
  if( !db->init.busy ){
    Vdbe *v;
    char *z;

    /* Make an entry in the sqlite_master table */
    v = sqlite3GetVdbe(pParse);
    if( v==0 ) goto triggerfinish_cleanup;
    sqlite3BeginWriteOperation(pParse, 0, iDb);
    z = sqlite3DbStrNDup(db, (char*)pAll->z, pAll->n);
    sqlite3NestedParse(pParse,
       "INSERT INTO %Q.%s VALUES('trigger',%Q,%Q,0,'CREATE TRIGGER %q')",
       db->aDb[iDb].zName, SCHEMA_TABLE(iDb), zName,
       pTrig->table, z);
    sqlite3DbFree(db, z);
    sqlite3ChangeCookie(pParse, iDb);
    sqlite3VdbeAddOp4(v, OP_ParseSchema, iDb, 0, 0, sqlite3MPrintf(
        db, "type='trigger' AND name='%q'", zName), P4_DYNAMIC
    );
  }

  if( db->init.busy ){
    Trigger *pLink = pTrig;
    Hash *pHash = &db->aDb[iDb].pSchema->trigHash;
    pTrig = sqlite3HashInsert(pHash, zName, sqlite3Strlen30(zName), pTrig);
    if( pTrig ){
      db->mallocFailed = 1;
    }else if( pLink->pSchema==pLink->pTabSchema ){
      Table *pTab;
      int n = sqlite3Strlen30(pLink->table);
      pTab = sqlite3HashFind(&pLink->pTabSchema->tblHash, pLink->table, n);
      assert( pTab!=0 );
      pLink->pNext = pTab->pTrigger;
      pTab->pTrigger = pLink;
    }
  }

triggerfinish_cleanup:
  sqlite3DeleteTrigger(db, pTrig);
  assert( !pParse->pNewTrigger );
  sqlite3DeleteTriggerStep(db, pStepList);
}

/*
** Turn a SELECT statement (that the pSelect parameter points to) into
** a trigger step.  Return a pointer to a TriggerStep structure.
**
** The parser calls this routine when it finds a SELECT statement in
** body of a TRIGGER.  
*/
TriggerStep *sqlite3TriggerSelectStep(sqlite3 *db, Select *pSelect){
  TriggerStep *pTriggerStep = sqlite3DbMallocZero(db, sizeof(TriggerStep));
  if( pTriggerStep==0 ) {
    sqlite3SelectDelete(db, pSelect);
    return 0;
  }
  pTriggerStep->op = TK_SELECT;
  pTriggerStep->pSelect = pSelect;
  pTriggerStep->orconf = OE_Default;
  return pTriggerStep;
}

/*
** Allocate space to hold a new trigger step.  The allocated space
** holds both the TriggerStep object and the TriggerStep.target.z string.
**
** If an OOM error occurs, NULL is returned and db->mallocFailed is set.
*/
static TriggerStep *triggerStepAllocate(
  sqlite3 *db,                /* Database connection */
  u8 op,                      /* Trigger opcode */
  Token *pName                /* The target name */
){
  TriggerStep *pTriggerStep;

  pTriggerStep = sqlite3DbMallocZero(db, sizeof(TriggerStep) + pName->n);
  if( pTriggerStep ){
    char *z = (char*)&pTriggerStep[1];
    memcpy(z, pName->z, pName->n);
    pTriggerStep->target.z = z;
    pTriggerStep->target.n = pName->n;
    pTriggerStep->op = op;
  }
  return pTriggerStep;
}

/*
** Build a trigger step out of an INSERT statement.  Return a pointer
** to the new trigger step.
**
** The parser calls this routine when it sees an INSERT inside the
** body of a trigger.
*/
TriggerStep *sqlite3TriggerInsertStep(
  sqlite3 *db,        /* The database connection */
  Token *pTableName,  /* Name of the table into which we insert */
  IdList *pColumn,    /* List of columns in pTableName to insert into */
  ExprList *pEList,   /* The VALUE clause: a list of values to be inserted */
  Select *pSelect,    /* A SELECT statement that supplies values */
  u8 orconf           /* The conflict algorithm (OE_Abort, OE_Replace, etc.) */
){
  TriggerStep *pTriggerStep;

  assert(pEList == 0 || pSelect == 0);
  assert(pEList != 0 || pSelect != 0 || db->mallocFailed);

  pTriggerStep = triggerStepAllocate(db, TK_INSERT, pTableName);
  if( pTriggerStep ){
    pTriggerStep->pSelect = sqlite3SelectDup(db, pSelect, EXPRDUP_REDUCE);
    pTriggerStep->pIdList = pColumn;
    pTriggerStep->pExprList = sqlite3ExprListDup(db, pEList, EXPRDUP_REDUCE);
    pTriggerStep->orconf = orconf;
  }else{
    sqlite3IdListDelete(db, pColumn);
  }
  sqlite3ExprListDelete(db, pEList);
  sqlite3SelectDelete(db, pSelect);

  return pTriggerStep;
}

/*
** Construct a trigger step that implements an UPDATE statement and return
** a pointer to that trigger step.  The parser calls this routine when it
** sees an UPDATE statement inside the body of a CREATE TRIGGER.
*/
TriggerStep *sqlite3TriggerUpdateStep(
  sqlite3 *db,         /* The database connection */
  Token *pTableName,   /* Name of the table to be updated */
  ExprList *pEList,    /* The SET clause: list of column and new values */
  Expr *pWhere,        /* The WHERE clause */
  u8 orconf            /* The conflict algorithm. (OE_Abort, OE_Ignore, etc) */
){
  TriggerStep *pTriggerStep;

  pTriggerStep = triggerStepAllocate(db, TK_UPDATE, pTableName);
  if( pTriggerStep ){
    pTriggerStep->pExprList = sqlite3ExprListDup(db, pEList, EXPRDUP_REDUCE);
    pTriggerStep->pWhere = sqlite3ExprDup(db, pWhere, EXPRDUP_REDUCE);
    pTriggerStep->orconf = orconf;
  }
  sqlite3ExprListDelete(db, pEList);
  sqlite3ExprDelete(db, pWhere);
  return pTriggerStep;
}

/*
** Construct a trigger step that implements a DELETE statement and return
** a pointer to that trigger step.  The parser calls this routine when it
** sees a DELETE statement inside the body of a CREATE TRIGGER.
*/
TriggerStep *sqlite3TriggerDeleteStep(
  sqlite3 *db,            /* Database connection */
  Token *pTableName,      /* The table from which rows are deleted */
  Expr *pWhere            /* The WHERE clause */
){
  TriggerStep *pTriggerStep;

  pTriggerStep = triggerStepAllocate(db, TK_DELETE, pTableName);
  if( pTriggerStep ){
    pTriggerStep->pWhere = sqlite3ExprDup(db, pWhere, EXPRDUP_REDUCE);
    pTriggerStep->orconf = OE_Default;
  }
  sqlite3ExprDelete(db, pWhere);
  return pTriggerStep;
}

/* 
** Recursively delete a Trigger structure
*/
void sqlite3DeleteTrigger(sqlite3 *db, Trigger *pTrigger){
  if( pTrigger==0 ) return;
  sqlite3DeleteTriggerStep(db, pTrigger->step_list);
  sqlite3DbFree(db, pTrigger->name);
  sqlite3DbFree(db, pTrigger->table);
  sqlite3ExprDelete(db, pTrigger->pWhen);
  sqlite3IdListDelete(db, pTrigger->pColumns);
  sqlite3DbFree(db, pTrigger);
}

/*
** This function is called to drop a trigger from the database schema. 
**
** This may be called directly from the parser and therefore identifies
** the trigger by name.  The sqlite3DropTriggerPtr() routine does the
** same job as this routine except it takes a pointer to the trigger
** instead of the trigger name.
**/
void sqlite3DropTrigger(Parse *pParse, SrcList *pName, int noErr){
  Trigger *pTrigger = 0;
  int i;
  const char *zDb;
  const char *zName;
  int nName;
  sqlite3 *db = pParse->db;

  if( db->mallocFailed ) goto drop_trigger_cleanup;
  if( SQLITE_OK!=sqlite3ReadSchema(pParse) ){
    goto drop_trigger_cleanup;
  }

  assert( pName->nSrc==1 );
  zDb = pName->a[0].zDatabase;
  zName = pName->a[0].zName;
  nName = sqlite3Strlen30(zName);
  for(i=OMIT_TEMPDB; i<db->nDb; i++){
    int j = (i<2) ? i^1 : i;  /* Search TEMP before MAIN */
    if( zDb && sqlite3StrICmp(db->aDb[j].zName, zDb) ) continue;
    pTrigger = sqlite3HashFind(&(db->aDb[j].pSchema->trigHash), zName, nName);
    if( pTrigger ) break;
  }
  if( !pTrigger ){
    if( !noErr ){
      sqlite3ErrorMsg(pParse, "no such trigger: %S", pName, 0);
    }
    goto drop_trigger_cleanup;
  }
  sqlite3DropTriggerPtr(pParse, pTrigger);

drop_trigger_cleanup:
  sqlite3SrcListDelete(db, pName);
}

/*
** Return a pointer to the Table structure for the table that a trigger
** is set on.
*/
static Table *tableOfTrigger(Trigger *pTrigger){
  int n = sqlite3Strlen30(pTrigger->table);
  return sqlite3HashFind(&pTrigger->pTabSchema->tblHash, pTrigger->table, n);
}


/*
** Drop a trigger given a pointer to that trigger. 
*/
void sqlite3DropTriggerPtr(Parse *pParse, Trigger *pTrigger){
  Table   *pTable;
  Vdbe *v;
  sqlite3 *db = pParse->db;
  int iDb;

  iDb = sqlite3SchemaToIndex(pParse->db, pTrigger->pSchema);
  assert( iDb>=0 && iDb<db->nDb );
  pTable = tableOfTrigger(pTrigger);
  assert( pTable );
  assert( pTable->pSchema==pTrigger->pSchema || iDb==1 );
#ifndef SQLITE_OMIT_AUTHORIZATION
  {
    int code = SQLITE_DROP_TRIGGER;
    const char *zDb = db->aDb[iDb].zName;
    const char *zTab = SCHEMA_TABLE(iDb);
    if( iDb==1 ) code = SQLITE_DROP_TEMP_TRIGGER;
    if( sqlite3AuthCheck(pParse, code, pTrigger->name, pTable->zName, zDb) ||
      sqlite3AuthCheck(pParse, SQLITE_DELETE, zTab, 0, zDb) ){
      return;
    }
  }
#endif

  /* Generate code to destroy the database record of the trigger.
  */
  assert( pTable!=0 );
  if( (v = sqlite3GetVdbe(pParse))!=0 ){
    int base;
    static const VdbeOpList dropTrigger[] = {
      { OP_Rewind,     0, ADDR(9),  0},
      { OP_String8,    0, 1,        0}, /* 1 */
      { OP_Column,     0, 1,        2},
      { OP_Ne,         2, ADDR(8),  1},
      { OP_String8,    0, 1,        0}, /* 4: "trigger" */
      { OP_Column,     0, 0,        2},
      { OP_Ne,         2, ADDR(8),  1},
      { OP_Delete,     0, 0,        0},
      { OP_Next,       0, ADDR(1),  0}, /* 8 */
    };

    sqlite3BeginWriteOperation(pParse, 0, iDb);
    sqlite3OpenMasterTable(pParse, iDb);
    base = sqlite3VdbeAddOpList(v,  ArraySize(dropTrigger), dropTrigger);
    sqlite3VdbeChangeP4(v, base+1, pTrigger->name, 0);
    sqlite3VdbeChangeP4(v, base+4, "trigger", P4_STATIC);
    sqlite3ChangeCookie(pParse, iDb);
    sqlite3VdbeAddOp2(v, OP_Close, 0, 0);
    sqlite3VdbeAddOp4(v, OP_DropTrigger, iDb, 0, 0, pTrigger->name, 0);
    if( pParse->nMem<3 ){
      pParse->nMem = 3;
    }
  }
}

/*
** Remove a trigger from the hash tables of the sqlite* pointer.
*/
void sqlite3UnlinkAndDeleteTrigger(sqlite3 *db, int iDb, const char *zName){
  Hash *pHash = &(db->aDb[iDb].pSchema->trigHash);
  Trigger *pTrigger;
  pTrigger = sqlite3HashInsert(pHash, zName, sqlite3Strlen30(zName), 0);
  if( ALWAYS(pTrigger) ){
    if( pTrigger->pSchema==pTrigger->pTabSchema ){
      Table *pTab = tableOfTrigger(pTrigger);
      Trigger **pp;
      for(pp=&pTab->pTrigger; *pp!=pTrigger; pp=&((*pp)->pNext));
      *pp = (*pp)->pNext;
    }
    sqlite3DeleteTrigger(db, pTrigger);
    db->flags |= SQLITE_InternChanges;
  }
}

/*
** pEList is the SET clause of an UPDATE statement.  Each entry
** in pEList is of the format <id>=<expr>.  If any of the entries
** in pEList have an <id> which matches an identifier in pIdList,
** then return TRUE.  If pIdList==NULL, then it is considered a
** wildcard that matches anything.  Likewise if pEList==NULL then
** it matches anything so always return true.  Return false only
** if there is no match.
*/
static int checkColumnOverlap(IdList *pIdList, ExprList *pEList){
  int e;
  if( pIdList==0 || NEVER(pEList==0) ) return 1;
  for(e=0; e<pEList->nExpr; e++){
    if( sqlite3IdListIndex(pIdList, pEList->a[e].zName)>=0 ) return 1;
  }
  return 0; 
}

/*
** Return a list of all triggers on table pTab if there exists at least
** one trigger that must be fired when an operation of type 'op' is 
** performed on the table, and, if that operation is an UPDATE, if at
** least one of the columns in pChanges is being modified.
*/
Trigger *sqlite3TriggersExist(
  Parse *pParse,          /* Parse context */
  Table *pTab,            /* The table the contains the triggers */
  int op,                 /* one of TK_DELETE, TK_INSERT, TK_UPDATE */
  ExprList *pChanges,     /* Columns that change in an UPDATE statement */
  int *pMask              /* OUT: Mask of TRIGGER_BEFORE|TRIGGER_AFTER */
){
  int mask = 0;
  Trigger *pList = sqlite3TriggerList(pParse, pTab);
  Trigger *p;
  assert( pList==0 || IsVirtual(pTab)==0 );
  for(p=pList; p; p=p->pNext){
    if( p->op==op && checkColumnOverlap(p->pColumns, pChanges) ){
      mask |= p->tr_tm;
    }
  }
  if( pMask ){
    *pMask = mask;
  }
  return (mask ? pList : 0);
}

/*
** Convert the pStep->target token into a SrcList and return a pointer
** to that SrcList.
**
** This routine adds a specific database name, if needed, to the target when
** forming the SrcList.  This prevents a trigger in one database from
** referring to a target in another database.  An exception is when the
** trigger is in TEMP in which case it can refer to any other database it
** wants.
*/
static SrcList *targetSrcList(
  Parse *pParse,       /* The parsing context */
  TriggerStep *pStep   /* The trigger containing the target token */
){
  int iDb;             /* Index of the database to use */
  SrcList *pSrc;       /* SrcList to be returned */

  pSrc = sqlite3SrcListAppend(pParse->db, 0, &pStep->target, 0);
  if( pSrc ){
    assert( pSrc->nSrc>0 );
    assert( pSrc->a!=0 );
    iDb = sqlite3SchemaToIndex(pParse->db, pStep->pTrig->pSchema);
    if( iDb==0 || iDb>=2 ){
      sqlite3 *db = pParse->db;
      assert( iDb<pParse->db->nDb );
      pSrc->a[pSrc->nSrc-1].zDatabase = sqlite3DbStrDup(db, db->aDb[iDb].zName);
    }
  }
  return pSrc;
}

/*
** Generate VDBE code for zero or more statements inside the body of a
** trigger.  
*/
static int codeTriggerProgram(
  Parse *pParse,            /* The parser context */
  TriggerStep *pStepList,   /* List of statements inside the trigger body */
  int orconfin              /* Conflict algorithm. (OE_Abort, etc) */  
){
  TriggerStep * pTriggerStep = pStepList;
  int orconf;
  Vdbe *v = pParse->pVdbe;
  sqlite3 *db = pParse->db;

  assert( pTriggerStep!=0 );
  assert( v!=0 );
  sqlite3VdbeAddOp2(v, OP_ContextPush, 0, 0);
  VdbeComment((v, "begin trigger %s", pStepList->pTrig->name));
  while( pTriggerStep ){
    sqlite3ExprCacheClear(pParse);
    orconf = (orconfin == OE_Default)?pTriggerStep->orconf:orconfin;
    pParse->trigStack->orconf = orconf;
    switch( pTriggerStep->op ){
      case TK_UPDATE: {
        SrcList *pSrc;
        pSrc = targetSrcList(pParse, pTriggerStep);
        sqlite3VdbeAddOp2(v, OP_ResetCount, 0, 0);
        sqlite3Update(pParse, pSrc,
                sqlite3ExprListDup(db, pTriggerStep->pExprList, 0), 
                sqlite3ExprDup(db, pTriggerStep->pWhere, 0), orconf);
        sqlite3VdbeAddOp2(v, OP_ResetCount, 1, 0);
        break;
      }
      case TK_INSERT: {
        SrcList *pSrc;
        pSrc = targetSrcList(pParse, pTriggerStep);
        sqlite3VdbeAddOp2(v, OP_ResetCount, 0, 0);
        sqlite3Insert(pParse, pSrc,
          sqlite3ExprListDup(db, pTriggerStep->pExprList, 0), 
          sqlite3SelectDup(db, pTriggerStep->pSelect, 0), 
          sqlite3IdListDup(db, pTriggerStep->pIdList), orconf);
        sqlite3VdbeAddOp2(v, OP_ResetCount, 1, 0);
        break;
      }
      case TK_DELETE: {
        SrcList *pSrc;
        sqlite3VdbeAddOp2(v, OP_ResetCount, 0, 0);
        pSrc = targetSrcList(pParse, pTriggerStep);
        sqlite3DeleteFrom(pParse, pSrc, 
                          sqlite3ExprDup(db, pTriggerStep->pWhere, 0));
        sqlite3VdbeAddOp2(v, OP_ResetCount, 1, 0);
        break;
      }
      default: assert( pTriggerStep->op==TK_SELECT ); {
        Select *ss = sqlite3SelectDup(db, pTriggerStep->pSelect, 0);
        if( ss ){
          SelectDest dest;

          sqlite3SelectDestInit(&dest, SRT_Discard, 0);
          sqlite3Select(pParse, ss, &dest);
          sqlite3SelectDelete(db, ss);
        }
        break;
      }
    } 
    pTriggerStep = pTriggerStep->pNext;
  }
  sqlite3VdbeAddOp2(v, OP_ContextPop, 0, 0);
  VdbeComment((v, "end trigger %s", pStepList->pTrig->name));

  return 0;
}

/*
** This is called to code FOR EACH ROW triggers.
**
** When the code that this function generates is executed, the following 
** must be true:
**
** 1. No cursors may be open in the main database.  (But newIdx and oldIdx
**    can be indices of cursors in temporary tables.  See below.)
**
** 2. If the triggers being coded are ON INSERT or ON UPDATE triggers, then
**    a temporary vdbe cursor (index newIdx) must be open and pointing at
**    a row containing values to be substituted for new.* expressions in the
**    trigger program(s).
**
** 3. If the triggers being coded are ON DELETE or ON UPDATE triggers, then
**    a temporary vdbe cursor (index oldIdx) must be open and pointing at
**    a row containing values to be substituted for old.* expressions in the
**    trigger program(s).
**
** If they are not NULL, the piOldColMask and piNewColMask output variables
** are set to values that describe the columns used by the trigger program
** in the OLD.* and NEW.* tables respectively. If column N of the 
** pseudo-table is read at least once, the corresponding bit of the output
** mask is set. If a column with an index greater than 32 is read, the
** output mask is set to the special value 0xffffffff.
**
*/
int sqlite3CodeRowTrigger(
  Parse *pParse,       /* Parse context */
  Trigger *pTrigger,   /* List of triggers on table pTab */
  int op,              /* One of TK_UPDATE, TK_INSERT, TK_DELETE */
  ExprList *pChanges,  /* Changes list for any UPDATE OF triggers */
  int tr_tm,           /* One of TRIGGER_BEFORE, TRIGGER_AFTER */
  Table *pTab,         /* The table to code triggers from */
  int newIdx,          /* The indice of the "new" row to access */
  int oldIdx,          /* The indice of the "old" row to access */
  int orconf,          /* ON CONFLICT policy */
  int ignoreJump,      /* Instruction to jump to for RAISE(IGNORE) */
  u32 *piOldColMask,   /* OUT: Mask of columns used from the OLD.* table */
  u32 *piNewColMask    /* OUT: Mask of columns used from the NEW.* table */
){
  Trigger *p;
  sqlite3 *db = pParse->db;
  TriggerStack trigStackEntry;

  trigStackEntry.oldColMask = 0;
  trigStackEntry.newColMask = 0;

  assert(op == TK_UPDATE || op == TK_INSERT || op == TK_DELETE);
  assert(tr_tm == TRIGGER_BEFORE || tr_tm == TRIGGER_AFTER );

  assert(newIdx != -1 || oldIdx != -1);

  for(p=pTrigger; p; p=p->pNext){
    int fire_this = 0;

    /* Sanity checking:  The schema for the trigger and for the table are
    ** always defined.  The trigger must be in the same schema as the table
    ** or else it must be a TEMP trigger. */
    assert( p->pSchema!=0 );
    assert( p->pTabSchema!=0 );
    assert( p->pSchema==p->pTabSchema || p->pSchema==db->aDb[1].pSchema );

    /* Determine whether we should code this trigger */
    if( 
      p->op==op && 
      p->tr_tm==tr_tm && 
      checkColumnOverlap(p->pColumns,pChanges)
    ){
      TriggerStack *pS;      /* Pointer to trigger-stack entry */
      for(pS=pParse->trigStack; pS && p!=pS->pTrigger; pS=pS->pNext){}
      if( !pS ){
        fire_this = 1;
      }
#if 0    /* Give no warning for recursive triggers.  Just do not do them */
      else{
        sqlite3ErrorMsg(pParse, "recursive triggers not supported (%s)",
            p->name);
        return SQLITE_ERROR;
      }
#endif
    }
 
    if( fire_this ){
      int endTrigger;
      Expr * whenExpr;
      AuthContext sContext;
      NameContext sNC;

#ifndef SQLITE_OMIT_TRACE
      sqlite3VdbeAddOp4(pParse->pVdbe, OP_Trace, 0, 0, 0,
                        sqlite3MPrintf(db, "-- TRIGGER %s", p->name),
                        P4_DYNAMIC);
#endif
      memset(&sNC, 0, sizeof(sNC));
      sNC.pParse = pParse;

      /* Push an entry on to the trigger stack */
      trigStackEntry.pTrigger = p;
      trigStackEntry.newIdx = newIdx;
      trigStackEntry.oldIdx = oldIdx;
      trigStackEntry.pTab = pTab;
      trigStackEntry.pNext = pParse->trigStack;
      trigStackEntry.ignoreJump = ignoreJump;
      pParse->trigStack = &trigStackEntry;
      sqlite3AuthContextPush(pParse, &sContext, p->name);

      /* code the WHEN clause */
      endTrigger = sqlite3VdbeMakeLabel(pParse->pVdbe);
      whenExpr = sqlite3ExprDup(db, p->pWhen, 0);
      if( db->mallocFailed || sqlite3ResolveExprNames(&sNC, whenExpr) ){
        pParse->trigStack = trigStackEntry.pNext;
        sqlite3ExprDelete(db, whenExpr);
        return 1;
      }
      sqlite3ExprIfFalse(pParse, whenExpr, endTrigger, SQLITE_JUMPIFNULL);
      sqlite3ExprDelete(db, whenExpr);

      codeTriggerProgram(pParse, p->step_list, orconf); 

      /* Pop the entry off the trigger stack */
      pParse->trigStack = trigStackEntry.pNext;
      sqlite3AuthContextPop(&sContext);

      sqlite3VdbeResolveLabel(pParse->pVdbe, endTrigger);
    }
  }
  if( piOldColMask ) *piOldColMask |= trigStackEntry.oldColMask;
  if( piNewColMask ) *piNewColMask |= trigStackEntry.newColMask;
  return 0;
}
#endif /* !defined(SQLITE_OMIT_TRIGGER) */
