/*
** LCLint - annotation-assisted static program checker
** Copyright (C) 1994-2000 University of Virginia,
**         Massachusetts Institute of Technology
**
** This program is free software; you can redistribute it and/or modify it
** under the terms of the GNU General Public License as published by the
** Free Software Foundation; either version 2 of the License, or (at your
** option) any later version.
** 
** This program is distributed in the hope that it will be useful, but
** WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
** General Public License for more details.
** 
** The GNU General Public License is available from http://www.gnu.org/ or
** the Free Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
** MA 02111-1307, USA.
**
** For information on lclint: lclint-request@cs.virginia.edu
** To report a bug: lclint-bug@cs.virginia.edu
** For more information: http://lclint.cs.virginia.edu
*/
/*
** context.c
*/
/*
 * Modified by Herbert 04/19/97:
 * - added include for new header portab.h containing OS dependent stuff.
 * - changed occurrances of '/' as path delimiters to macro from portab.h
 * - changed the handling of the tmp directory -- will no longer always be
 *   ".", if possible, environment variables "TMP" or, if not set, "TMP", 
 *   or, if not set "." will be used.
 */

# include "lclintMacros.nf"
# include "llbasic.h"

# ifndef NOLCL
# include "usymtab_interface.h"
# endif

# include "exprChecks.h"
# include "filelocStack.h"
# include "fileIdList.h"
# include "llmain.h"
# include "intSet.h"
# include "osd.h"
# include "portab.h"

extern /*@external@*/ int yydebug;

typedef struct
{ 
  cstring file; 
  typeIdSet daccess ; 
} maccesst;

typedef enum { 
  CX_GLOBAL, CX_INNER, 
  CX_FUNCTION, CX_FCNDECL,
  CX_MACROFCN, CX_MACROCONST, CX_UNKNOWNMACRO, 
  CX_ITERDEF, CX_ITEREND,
  CX_LCL, CX_LCLLIB
} kcontext;

static struct _context
{
  int linesprocessed;
  int speclinesprocessed;

  flagMarkerList markers;

  /*
  **  used to record state where a macro must match a function name
  **   (i.e., no params were listed in the macro definition
  */

  bool macroMissingParams BOOLBITS;
  bool preprocessing BOOLBITS;
  bool incommandline BOOLBITS;
  bool insuppressregion  BOOLBITS;
  bool inDerivedFile BOOLBITS;
  bool instandardlib  BOOLBITS;
  bool inimport  BOOLBITS;
  bool inheader  BOOLBITS;
  bool inmacrocache  BOOLBITS;
  bool protectVars  BOOLBITS;
  bool neednl  BOOLBITS;
  bool showfunction  BOOLBITS;
  bool savedFlags  BOOLBITS;
  bool justpopped  BOOLBITS;
  bool anyExports BOOLBITS;

  flagcode library;

  ynm isNullGuarded;
  fileloc saveloc;
  fileloc pushloc;

  clauseStack clauses; 
  clause inclause;

  int numerrors;

  filelocStack locstack;
  fileTable ftab;
  cstring msgAnnote;
  /*@observer@*/ sRef aliasAnnote;
  /*@observer@*/ sRef aliasAnnoteAls;
  messageLog  msgLog;

  macrocache  mc;
  /*@observer@*/ sRefSet mods;

  /* file access types */
  typeIdSet facct;   

  /* local access types (this function) */
  typeIdSet acct;  

  /* no access types (@noaccess) */
  typeIdSet nacct; 

  /*@observer@*/ globSet globs;
  /*@only@*/ globSet globs_used;
  
  int nmods;
  int maxmods;
  /*@reldef@*/ maccesst *moduleaccess; /* Not defined is nmods == 0. */
  
  kcontext kind;

  ctype boolType;

  bool flags[NUMFLAGS];
  bool saveflags[NUMFLAGS];
  bool setGlobally[NUMFLAGS];
  bool setLocally[NUMFLAGS];

  int values[NUMVALUEFLAGS];
  int counters[NUMVALUEFLAGS];

  o_cstring strings[NUMSTRINGFLAGS];
  sRefSetList modrecs;

  union 
    {
      bool glob;
      int  cdepth;
      /*@dependent@*/ /*@exposed@*/ uentry  fcn;
    } cont;
} gc;

static /*@exposed@*/ cstring context_exposeString (flagcode p_flag) ;
static void context_restoreFlagSettings (void) /*@modifies gc@*/ ;
static void context_saveFlagSettings (void) /*@modifies gc@*/ ;
static void context_exitClauseAux (exprNode p_pred, exprNode p_tbranch)
   /*@modifies gc@*/ ;
static void context_exitClauseSimp (void)  /*@modifies gc@*/ ;
static void context_exitClausePlain (void) /*@modifies gc@*/ ;
static void context_setJustPopped (void) /*@modifies gc.justpopped@*/ ;
static void context_setValue (flagcode p_flag, int p_val) /*@modifies gc.flags@*/ ;
static void context_setFlag (flagcode p_f, bool p_b)
  /*@modifies gc.flags@*/ ;

static void
  context_setFlagAux (flagcode p_f, bool p_b, bool p_inFile, bool p_isRestore)
  /*@modifies gc.flags@*/ ;

static void context_restoreFlag (flagcode p_f) /*@modifies gc.flags@*/ ;

/*@+enumindex@*/ 

cstring context_unparseFlagMarkers ()
{
  return (flagMarkerList_unparse (gc.markers));
}

void context_setPreprocessing (void)
{
  llassert (!gc.preprocessing);
  gc.preprocessing = TRUE;
}

void context_clearPreprocessing (void)
{
  llassert (gc.preprocessing);
  gc.preprocessing = FALSE;
}

bool context_isPreprocessing (void)
{
  return gc.preprocessing;
}

void context_setInCommandLine (void)
{
  llassert (!gc.incommandline);
  gc.incommandline = TRUE;
}

void context_clearInCommandLine (void)
{
  llassert (gc.incommandline);
  gc.incommandline = FALSE;
}

bool context_isInCommandLine (void)
{
  return gc.incommandline;
}

static
void pushClause (clause c) /*@modifies gc.clauses, gc.inclause@*/
{
  gc.inclause = c;
  clauseStack_push (gc.clauses, c);

  if (clause_isConditional (c)
      && context_getFlag (FLG_CONTROLNESTDEPTH))
    {
      int maxdepth = context_getValue (FLG_CONTROLNESTDEPTH);
      int depth = clauseStack_controlDepth (gc.clauses);
      
      if (depth == maxdepth + 1)
	{
	  voptgenerror 
	    (FLG_CONTROLNESTDEPTH,
	     message ("Maximum control nesting depth "
		      "(%d) exceeded",
		      maxdepth),
	     g_currentloc);
	}
    }
}

static
clause topClause (clauseStack s) /*@*/
{
  if (clauseStack_isEmpty (s)) return NOCLAUSE;
  return ((clause) clauseStack_top (s));
}

void
context_addMacroCache (/*@only@*/ cstring def)
{
    macrocache_addEntry (gc.mc, fileloc_copy (g_currentloc), def);
}

void
context_addComment (/*@only@*/ cstring def)
{
    macrocache_addComment (gc.mc, fileloc_copy (g_currentloc), def);
}

/*
** returns TRUE is fl is in ignore region, or region where -code
**
** the logic is fuzzy...
*/

static bool
context_inSuppressFlagZone (fileloc fl, flagcode code)
{
  ynm ret = flagMarkerList_suppressError (gc.markers, code, fl);
  bool res = FALSE;
  
  if (ynm_isMaybe (ret))
    {
      /*
      ** whas is dis?
      */

      if (gc.savedFlags)
	{
	  res = !gc.saveflags[code];
	}
      else
	{
	  res = !context_getFlag (code);
	}
    }
  else
    {
      res = ynm_toBoolStrict (ret);
    }
  
    return res;
}

static bool
context_suppressSystemMsg (fileloc fl)
{
  if (context_getFlag (FLG_SYSTEMDIRERRORS))
    {
      return FALSE;
    }
  else
    {
      return (fileloc_isSystemFile (fl));
    }
}

bool 
context_suppressFlagMsg (flagcode flag, fileloc fl)
{
  if (context_suppressSystemMsg (fl))
    {
      return TRUE;
    }

  /* want same object compare here */

  if (fileloc_equal (fl, g_currentloc) || gc.inDerivedFile)
    {
      return (!context_getFlag (flag)
	      || context_inSuppressRegion ()
	      || context_inSuppressZone (fl)
	      || (gc.inDerivedFile && context_inSuppressFlagZone (fl, flag)));
    }
  else
    {
            return (context_inSuppressFlagZone (fl, flag));
    }
}

bool 
context_suppressNotFlagMsg (flagcode flag, fileloc fl)
{
  
  if (context_suppressSystemMsg (fl))
    {
      return TRUE;
    }

  /*@access fileloc@*/ 
  if (fl == g_currentloc)
    /*@noaccess fileloc@*/
    {
      return (context_getFlag (flag) || context_inSuppressRegion ());
    }
  else
    {
      /* for now... */
      return (context_getFlag (flag) || context_inSuppressRegion ());
    }
}

bool
context_inSuppressZone (fileloc fl) 
{
  if (context_suppressSystemMsg (fl))
    {
      return TRUE;
    }

  return (flagMarkerList_inIgnore (gc.markers, fl));
}

bool
context_inSuppressRegion (void)
{
  return (gc.insuppressregion);
}

void
context_enterSuppressRegion (void)
{
  if (gc.insuppressregion)
    {
      gc.insuppressregion = FALSE;	/* get this msg! */
      llmsg (message
	     ("%q: New ignore errors region entered while in ignore errors region",
	      fileloc_unparse (g_currentloc)));
    }
  
  gc.insuppressregion = TRUE;
  flagMarkerList_add (gc.markers, flagMarker_createIgnoreOn (g_currentloc));
}

static void
context_addFlagMarker (flagcode code, ynm set)
{
  flagMarkerList_add (gc.markers,
		      flagMarker_createLocalSet (code, set, g_currentloc));
}

void
context_enterSuppressLine (int count)
{
  fileloc nextline = fileloc_copy (g_currentloc);

  flagMarkerList_add (gc.markers,
		      flagMarker_createIgnoreCount (count, g_currentloc));

  fileloc_nextLine (nextline);
  flagMarkerList_add (gc.markers,
		      flagMarker_createIgnoreOff (nextline));
  fileloc_free (nextline);
}

void context_checkSuppressCounts (void)
{
  if (context_getFlag (FLG_SUPCOUNTS))
    {
      flagMarkerList_checkSuppressCounts (gc.markers);
    }
}

void context_incLineno (void)
{
  gc.linesprocessed++;
  incLine ();
}

void
context_exitSuppressRegion (void)
{
  if (!gc.insuppressregion)
    {
      llerrorlit (FLG_SYNTAX, 
		  "End ignore errors in region while not ignoring errors");
    }

    gc.insuppressregion = FALSE;
  flagMarkerList_add (gc.markers, flagMarker_createIgnoreOff (g_currentloc));
}

# ifndef NOLCL
void
context_enterLCLfile (void)
{
  gc.kind = CX_LCL;
  gc.facct = typeIdSet_emptySet ();
}
# endif

static void
addModuleAccess (/*@only@*/ cstring fname, typeIdSet mods)
{
  int i;

  for (i = 0; i < gc.nmods; i++)
    {
      if (cstring_equal (gc.moduleaccess[i].file, fname))
	{
	  gc.moduleaccess[i].daccess = typeIdSet_union (gc.moduleaccess[i].daccess, mods);
	  cstring_free (fname);
	  return;
	}
    }
  
  if (gc.nmods == gc.maxmods)
    {
      maccesst *oldmods;
      
      gc.maxmods = gc.maxmods + DEFAULTMAXMODS;      
      oldmods = gc.moduleaccess;
      
      gc.moduleaccess = (maccesst *) dmalloc (sizeof (*gc.moduleaccess) * (gc.maxmods));
      
      for (i = 0; i < gc.nmods; i++)
	{
	  gc.moduleaccess[i] = oldmods[i];
	}
      
      sfree (oldmods);
    }
  
  gc.moduleaccess[gc.nmods].file = fname;
  gc.moduleaccess[gc.nmods].daccess = mods;
  
  gc.nmods++;
}

static void
insertModuleAccess (cstring fname, typeId t)
{
  int i;
  
  for (i = 0; i < gc.nmods; i++)
    {
      if (cstring_equal (gc.moduleaccess[i].file, fname))
	{
	  gc.moduleaccess[i].daccess = typeIdSet_insert (gc.moduleaccess[i].daccess, t);
	  break;
	}
    }
  
    addModuleAccess (cstring_copy (fname), typeIdSet_single (t));
}

# ifndef NOLCL
void
context_exitLCLfile (void)
{
  if (gc.kind != CX_LCLLIB)
    {
      char *lclname =  
	removeExtension (cstring_toCharsSafe 
			 (fileName (currentFile ())), ".lcl"); 
      
      addModuleAccess (cstring_fromCharsO (removePath (lclname)), gc.facct);
      
      mstring_free (lclname);
    }
  
  gc.kind = CX_LCL;
  gc.kind = CX_GLOBAL;
  gc.facct = typeIdSet_emptySet ();
}
# endif

void
context_dumpModuleAccess (FILE *fout)
{
  int i = 0;

  for (i = 0; i < gc.nmods; i++)
    {
      cstring td = typeIdSet_dump (gc.moduleaccess[i].daccess);

      fprintf (fout, "%s#%s@\n", 
	       cstring_toCharsSafe (gc.moduleaccess[i].file), 
	       cstring_toCharsSafe (td));
      
      cstring_free (td);
    }
}

bool context_usingPosixLibrary ()
{
  return (gc.library == FLG_POSIXLIB 
	  || gc.library == FLG_POSIXSTRICTLIB
	  || gc.library == FLG_UNIXLIB
	  || gc.library == FLG_UNIXSTRICTLIB);
}

bool context_usingAnsiLibrary ()
{
  return (gc.library != FLG_NOLIB);
}

flagcode context_getLibrary ()
{
  return gc.library;
}

void context_setLibrary (flagcode code)
{
  gc.library = code;
}

/*@observer@*/ char *context_selectedLibrary ()
{
  switch (gc.library)
    {
    case FLG_STRICTLIB:
      return LLSTRICTLIBS_NAME;
    case FLG_POSIXLIB:
      return LLPOSIXLIBS_NAME;
    case FLG_POSIXSTRICTLIB:
      return LLPOSIXSTRICTLIBS_NAME;
    case FLG_UNIXLIB:
      return LLUNIXLIBS_NAME;    
    case FLG_UNIXSTRICTLIB:
      return LLUNIXSTRICTLIBS_NAME;
    case FLG_ANSILIB:
      return LLSTDLIBS_NAME;
    BADDEFAULT;
    }
}
  

void
context_loadModuleAccess (FILE *in)
{
  char *s = mstring_create (MAX_DUMP_LINE_LENGTH);
  char *lasts = s;
  char *name = mstring_create (MAX_NAME_LENGTH);
  char *oname = name;
# ifndef NOFREE
  char *os = s;
# endif

  while (fgets (s, MAX_DUMP_LINE_LENGTH, in) != NULL 
	 && *s == ';')
    {
      ;
    }

  while (s != NULL && *s != ';' && *s != '\0')
    {
      name = oname;
      
      while (*s != '#' && *s != '\0')
	{
	  *name++ = *s++;
	}

      *name = '\0';

      if (*s != '#')
	{
	  llcontbug (message ("context_loadModuleAccess: bad library line: %s\n", 
			      cstring_fromChars (s)));
	  break;
	}

      s++;

      addModuleAccess (cstring_copy (cstring_fromChars (oname)), 
		       typeIdSet_undump (&s)); 

      (void) fgets (s, MAX_DUMP_LINE_LENGTH, in);
      llassert (s != lasts);
      lasts = s;
    }

  sfree (oname);
# ifndef NOFREE
  sfree (os);
# endif
}

typeIdSet context_fileAccessTypes (void)
{
  return gc.facct;
}

void
context_resetModeFlags (void)
{
  
  allFlagCodes (code)
    {
      if (flagcode_isModeFlag (code))
	{
	  context_setFlag (code, FALSE);
	}
    } end_allFlagCodes;

  }

/*
** resetAllFlags
**
** Set all flags to FALSE, except for a few which are
** true by default.
**
** Set all values and strings to appropriate defaults.
** Set all counters to 0.
*/

static void
conext_resetAllCounters (void)
{
  int i;

  for (i = 0; i < NUMVALUEFLAGS; i++)
    {
      gc.counters[i] = 0;
    }
}

void
context_resetAllFlags (void)
{
  allFlagCodes (code)
    {
      gc.flags[code] = FALSE;

      if (flagcode_hasValue (code))
	{
	  int val = 0;
	  
	  /*@-loopswitchbreak@*/
	  switch (code)
	    {
	    case FLG_LIMIT: 
	      val = DEFAULT_LIMIT; break;
	    case FLG_LINELEN: 
	      val = DEFAULT_LINELEN; break;
	    case FLG_EXTERNALNAMELEN:
	      val = DEFAULT_EXTERNALNAMELEN; break;
	    case FLG_INTERNALNAMELEN:
	      val = DEFAULT_INTERNALNAMELEN; break;
	    case FLG_COMMENTCHAR: 
	      val = (int) DEFAULT_COMMENTCHAR; break;
	    case FLG_CONTROLNESTDEPTH:
	      val = (int) DEFAULT_CONTROLNESTDEPTH; break;
	    case FLG_STRINGLITERALLEN:
	      val = (int) DEFAULT_STRINGLITERALLEN; break;
	    case FLG_INCLUDENEST:
	      val = (int) DEFAULT_INCLUDENEST; break;
	    case FLG_NUMSTRUCTFIELDS:
	      val = (int) DEFAULT_NUMSTRUCTFIELDS; break;
	    case FLG_NUMENUMMEMBERS:
	      val = (int) DEFAULT_NUMENUMMEMBERS; break;
	    case FLG_EXPECT:
	    case FLG_LCLEXPECT:
	      break;
	    default:
	      llbug (message ("Bad value flag: %s", flagcode_unparse (code)));
	    }
	  /*@=loopswitchbreak@*/	  

	  context_setValue (code, val);
	}
      else if (flagcode_hasString (code))
	{
	  cstring val = cstring_undefined;
	  
	  switch (code)
	    { /*@-loopswitchbreak@*/
	    case FLG_LARCHPATH:
	      {
		char *larchpath = osd_getEnvironmentVariable (LARCH_PATH);
		
		if (larchpath != NULL)
		  {
		    val = cstring_fromCharsNew (larchpath);
		  }
		else
		  {
		    val = cstring_makeLiteral (DEFAULT_LARCHPATH);
		  }
		
		break;
	      }
	    case FLG_LCLIMPORTDIR:
	      {
		val = cstring_fromCharsNew (osd_getEnvironment (LCLIMPORTDIR, DEFAULT_LCLIMPORTDIR));
		break;
	      }
	    case FLG_TMPDIR: 
# if defined(OS2) || defined(MSDOS) || defined(WIN32)
              {
                char *env = osd_getEnvironmentVariable ("TMP");

                if (env == NULL)
                  {
                    env = osd_getEnvironmentVariable ("TEMP");
                  }

                val = cstring_makeLiteral (env != NULL ? env : DEFAULT_TMPDIR);
	      }
# else
	      val = cstring_makeLiteral (DEFAULT_TMPDIR);
# endif /* !defined(OS2) && !defined(MSDOS) */

	      break;
	    case FLG_BOOLTYPE:
	      val = cstring_makeLiteral (DEFAULT_BOOLTYPE); break;
	    case FLG_BOOLFALSE:
	      val = cstring_makeLiteral ("FALSE"); break;
	    case FLG_BOOLTRUE:
	      val = cstring_makeLiteral ("TRUE"); break;
	    case FLG_MACROVARPREFIX: 
	      val = cstring_makeLiteral ("m_"); break;
	    case FLG_SYSTEMDIRS:
	      val = cstring_makeLiteral (DEFAULT_SYSTEMDIR); break;
	    default:
	      break;
	    } /*@=loopswitchbreak@*/
	  
	  context_setString (code, val);
	}
      else
	{ 
	  ; /* nothing to set */
	}
    } end_allFlagCodes;
  
  /*
  ** These flags are true by default.
  */

  gc.flags[FLG_MODIFIES] = TRUE;
  gc.flags[FLG_NESTCOMMENT] = TRUE;
  gc.flags[FLG_GLOBALS] = TRUE;
  gc.flags[FLG_FULLINITBLOCK] = TRUE;
  gc.flags[FLG_LIKELYBOOL] = TRUE;
  gc.flags[FLG_ZEROPTR] = TRUE;
  gc.flags[FLG_NUMLITERAL] = TRUE;
  gc.flags[FLG_DUPLICATEQUALS] = TRUE;
  gc.flags[FLG_SKIPANSIHEADERS] = TRUE;
  gc.flags[FLG_SKIPPOSIXHEADERS] = TRUE;
  gc.flags[FLG_SYSTEMDIREXPAND] = TRUE;
  gc.flags[FLG_UNRECOGCOMMENTS] = TRUE;
  gc.flags[FLG_CASTFCNPTR] = TRUE;
  gc.flags[FLG_DOLCS] = TRUE;
  gc.flags[FLG_USEVARARGS] = TRUE;
  gc.flags[FLG_MAINTYPE] = TRUE;
  gc.flags[FLG_SPECMACROS] = TRUE;
  gc.flags[FLG_REDEF] = TRUE;
  gc.flags[FLG_MACRONEXTLINE] = TRUE;

  gc.flags[FLG_SIZEOFFORMALARRAY] = TRUE;
  gc.flags[FLG_FIXEDFORMALARRAY] = TRUE;

  gc.flags[FLG_PREDASSIGN] = TRUE;
  gc.flags[FLG_MODOBSERVER] = TRUE;
  gc.flags[FLG_MACROVARPREFIXEXCLUDE] = TRUE;
  gc.flags[FLG_EXTERNALNAMECASEINSENSITIVE] = TRUE;

  gc.flags[FLG_PARAMIMPTEMP] = TRUE;
  gc.flags[FLG_RETIMPONLY] = TRUE;
  gc.flags[FLG_GLOBIMPONLY] = TRUE;
  gc.flags[FLG_STRUCTIMPONLY] = TRUE;
  gc.flags[FLG_PREPROC] = TRUE;
  gc.flags[FLG_NAMECHECKS] = TRUE;
  gc.flags[FLG_FORMATCODE] = TRUE;
  gc.flags[FLG_FORMATTYPE] = TRUE;
  gc.flags[FLG_BADFLAG] = TRUE;
  gc.flags[FLG_WARNFLAGS] = TRUE;
  gc.flags[FLG_WARNUNIXLIB] = TRUE;
  gc.flags[FLG_WARNPOSIX] = TRUE;
  gc.flags[FLG_SHOWCOL] = TRUE;
  gc.flags[FLG_SHOWFUNC] = TRUE;
  gc.flags[FLG_SUPCOUNTS] = TRUE;
  gc.flags[FLG_HINTS] = TRUE;
  gc.flags[FLG_SYNTAX] = TRUE;
  gc.flags[FLG_TYPE] = TRUE;
  gc.flags[FLG_INCOMPLETETYPE] = TRUE;
  gc.flags[FLG_ABSTRACT] = TRUE;
  gc.flags[FLG_ITER] = TRUE;
  gc.flags[FLG_CONTROL] = TRUE;
  gc.flags[FLG_UNRECOG] = TRUE;
  gc.flags[FLG_SYSTEMUNRECOG] = TRUE;
  gc.flags[FLG_LINTCOMMENTS] = TRUE;
  gc.flags[FLG_ACCESSCZECH] = TRUE;
  gc.flags[FLG_ACCESSMODULE] = TRUE;
  gc.flags[FLG_ACCESSFILE] = TRUE;
  gc.flags[FLG_MACROVARPREFIX] = TRUE;

  /*
  ** Changed for version 2.4.
  */

  gc.flags[FLG_GNUEXTENSIONS] = TRUE;

  /*
  ** On by default for Win32, but not Unix (to support MS/VC++ error message format).
  */

# ifdef WIN32
  gc.flags[FLG_PARENFILEFORMAT] = TRUE;
# endif
}

/*
** C is way-lame, and you can initialize an array to a constant except where
** it is declared.  Hence, a macro is used to set the modeflags.
*/

/*@notfunction@*/
# define SETFLAGS() \
  { int i = 0; while (modeflags[i] != INVALID_FLAG) { \
      if (!flagcode_isModeFlag (modeflags[i])) \
	{ llbug (message ("not a mode flag: %s", \
			  flagcode_unparse (modeflags[i]))); } \
      else { context_setFlag (modeflags[i], TRUE); }  i++; }}

void
context_setMode (cstring s)
{
  intSet setflags = intSet_new ();
  
  allFlagCodes (code)
    {
      if (flagcode_isModeFlag (code))
	{
	  if (gc.setGlobally[code])
	    {
	      (void) intSet_insert (setflags, (int) code);
	    }
	}
    } end_allFlagCodes;
  
  if (!intSet_isEmpty (setflags))
    {
      cstring rflags = cstring_undefined;
      int num = 0;

      intSet_elements (setflags, el)
	{
	  if (cstring_isUndefined (rflags))
	    {
	      rflags = cstring_copy (flagcode_unparse ((flagcode) (el)));
	    }
	  else
	    {
	      rflags = message ("%q, %s", rflags, 
				flagcode_unparse ((flagcode) el));
	    }

	  num++;
	  if (num > 4 && intSet_size (setflags) > 6)
	    {
	      rflags = message ("%q, (%d others) ...", rflags, 
				intSet_size (setflags) - num);
	      break;
	    }
	} end_intSet_elements ;
      
      voptgenerror (FLG_WARNFLAGS,
		    message ("Setting mode %s after setting mode flags will "
			     "override set values of flags: %s",
			     s, rflags),
		    g_currentloc);

      cstring_free (rflags);
    }

  intSet_free (setflags);

  context_resetModeFlags ();

  if (cstring_equalLit (s, "standard"))
    {
      flagcode modeflags[] = 
	{
	  FLG_ENUMINT, FLG_MACROMATCHNAME,
	  FLG_MACROUNDEF, FLG_RELAXQUALS, 
	  FLG_USEALLGLOBS, FLG_CHECKSTRICTGLOBALS,
	  FLG_CHECKSTRICTGLOBALIAS,
	  FLG_CHECKEDGLOBALIAS,
	  FLG_CHECKMODGLOBALIAS,
	  FLG_PREDBOOLOTHERS, FLG_PREDBOOLINT,
	  FLG_PARAMUNUSED, FLG_VARUNUSED, FLG_FUNCUNUSED, 
	  FLG_TYPEUNUSED,
	  FLG_CONSTUNUSED, FLG_ENUMMEMUNUSED, FLG_FIELDUNUSED,
	  FLG_PTRNUMCOMPARE, FLG_BOOLCOMPARE, FLG_MUTREP, 
	  FLG_NOEFFECT, FLG_IMPTYPE,
	  FLG_RETVALOTHER, FLG_RETVALBOOL, FLG_RETVALINT,
	  FLG_SPECUNDEF, FLG_INCONDEFS, FLG_INCONDEFSLIB, 
	  FLG_MATCHFIELDS,
	  FLG_MACROPARAMS, FLG_MACROASSIGN, FLG_SEFPARAMS, 
	  FLG_MACROSTMT, FLG_MACROPARENS, 
	  FLG_MACROFCNDECL,
	  FLG_MACROCONSTDECL,
	  FLG_MACROREDEF, FLG_INFLOOPS, FLG_UNREACHABLE, 
	  FLG_NORETURN, FLG_CASEBREAK, FLG_MISSCASE, FLG_USEDEF, 
	  FLG_FIRSTCASE,
	  FLG_NESTEDEXTERN, 
	  FLG_NUMLITERAL,
	  FLG_ZEROBOOL,
	  /* memchecks flags */
	  FLG_NULLDEREF, 
	  FLG_NULLSTATE, FLG_NULLASSIGN,
 	  FLG_NULLPASS, FLG_NULLRET,        

	  FLG_COMPDEF, FLG_COMPMEMPASS, FLG_UNIONDEF,
	  FLG_RETSTACK,

	  /* memtrans flags */
	  FLG_EXPOSETRANS,
	  FLG_OBSERVERTRANS,
	  FLG_DEPENDENTTRANS,
	  FLG_NEWREFTRANS,
	  FLG_ONLYTRANS,
	  FLG_OWNEDTRANS,
	  FLG_FRESHTRANS,
	  FLG_SHAREDTRANS,
	  FLG_TEMPTRANS,
	  FLG_KEPTTRANS,
	  FLG_REFCOUNTTRANS,
	  FLG_STATICTRANS,
	  FLG_UNKNOWNTRANS,
	  FLG_KEEPTRANS,
	  FLG_IMMEDIATETRANS,

	  FLG_EXPORTLOCAL,

	  FLG_USERELEASED, FLG_ALIASUNIQUE, FLG_MAYALIASUNIQUE,
	  FLG_MUSTFREE, FLG_MUSTDEFINE, FLG_GLOBSTATE, 
	  FLG_COMPDESTROY, FLG_MUSTNOTALIAS,
	  FLG_MEMIMPLICIT,
	  FLG_BRANCHSTATE, 

	  FLG_EVALORDER, FLG_SHADOW, FLG_READONLYSTRINGS,
	  FLG_EXITARG,
	  FLG_IMPCHECKEDSPECGLOBALS,
	  FLG_MODGLOBS, FLG_WARNLINTCOMMENTS,
	  FLG_IFEMPTY, FLG_REALCOMPARE,
	  FLG_BOOLOPS, FLG_PTRNEGATE,
	  FLG_SHIFTSIGNED,
	  INVALID_FLAG } ;

      SETFLAGS ();
    }
  else if (cstring_equalLit (s, "weak"))
    {
      flagcode modeflags[] = 
	{ 
	  FLG_BOOLINT, FLG_CHARINT, FLG_FLOATDOUBLE,
	  FLG_ENUMINT, FLG_RELAXQUALS, FLG_FORWARDDECL, 
	  FLG_CHARINDEX, FLG_ABSTVOIDP, FLG_USEALLGLOBS, 
	  FLG_CHARUNSIGNEDCHAR,
	  FLG_PREDBOOLOTHERS, 
          FLG_VARUNUSED, FLG_FUNCUNUSED, 
	  FLG_TYPEUNUSED,
          FLG_CHECKSTRICTGLOBALS, FLG_MACROMATCHNAME,
	  FLG_RETVALOTHER,
	  FLG_IFEMPTY, 
	  FLG_RETSTACK, FLG_PTRNEGATE,
	  FLG_LONGUNSIGNEDINTEGRAL,
	  FLG_LONGUNSIGNEDUNSIGNEDINTEGRAL,
	  FLG_NUMLITERAL,
	  FLG_CHARINTLITERAL,
	  FLG_ZEROBOOL,
	  INVALID_FLAG 
	  } ;

      SETFLAGS ();
    }
  else if (cstring_equalLit (s, "checks"))
    {
      flagcode modeflags[] = 
	{ 
	  FLG_EXPORTLOCAL, FLG_IMPTYPE,
	  FLG_CHECKSTRICTGLOBALIAS,
	  FLG_CHECKEDGLOBALIAS,
	  FLG_CHECKMODGLOBALIAS,
	  FLG_UNCHECKEDGLOBALIAS,
          FLG_EXITARG, FLG_PTRNUMCOMPARE, 
	  FLG_BOOLCOMPARE, FLG_MACROUNDEF, 
	  FLG_MUSTMOD, FLG_ALLGLOBALS,
	  FLG_PREDBOOLOTHERS, FLG_PREDBOOLPTR, FLG_PREDBOOLINT,
	  FLG_USEALLGLOBS, FLG_MUTREP, FLG_RETALIAS, 
	  FLG_RETEXPOSE, FLG_ASSIGNEXPOSE, FLG_CASTEXPOSE,
	  FLG_FUNCUNUSED, FLG_GLOBALSIMPMODIFIESNOTHING,
	  FLG_TYPEUNUSED, FLG_FIELDUNUSED, FLG_PARAMUNUSED, FLG_VARUNUSED,
	  FLG_CONSTUNUSED, FLG_ENUMMEMUNUSED,
	  FLG_NOEFFECT, FLG_EXPORTHEADER, FLG_EXPORTHEADERVAR,
	  FLG_RETVALOTHER, FLG_RETVALBOOL, FLG_RETVALINT,
	  FLG_SPECUNDEF, FLG_IMPCHECKMODINTERNALS,
	  FLG_DECLUNDEF, FLG_INCONDEFS, FLG_INCONDEFSLIB, 
	  FLG_MATCHFIELDS, 
	  FLG_MACROPARAMS,
	  FLG_MACROASSIGN,
	  FLG_DECLPARAMMATCH,
	  FLG_FCNDEREF,
	  FLG_FIRSTCASE,
	  FLG_SEFPARAMS, FLG_SEFUNSPEC, FLG_MACROSTMT, FLG_MACROPARENS, 
	  FLG_MACROCONSTDECL,
	  FLG_MACROFCNDECL,
	  FLG_MACROREDEF, 
	  FLG_INFLOOPS, FLG_INFLOOPSUNCON,
	  FLG_UNREACHABLE, 
	  FLG_NORETURN, FLG_CASEBREAK, FLG_MISSCASE,
	  FLG_EVALORDER, FLG_USEDEF, 

	  FLG_NESTEDEXTERN, 

	  /* memchecks flags */

	  FLG_NULLSTATE, FLG_NULLDEREF, FLG_NULLASSIGN,
	  FLG_NULLPASS, FLG_NULLRET,

	  FLG_COMPDEF, FLG_COMPMEMPASS, FLG_UNIONDEF, FLG_RETSTACK,	  

	  /* memtrans flags */
	  FLG_EXPOSETRANS,
	  FLG_OBSERVERTRANS,
	  FLG_DEPENDENTTRANS,
	  FLG_NEWREFTRANS,
	  FLG_ONLYTRANS,
	  FLG_OWNEDTRANS,
	  FLG_FRESHTRANS,
	  FLG_SHAREDTRANS,
	  FLG_TEMPTRANS,
	  FLG_KEPTTRANS,
	  FLG_REFCOUNTTRANS,
	  FLG_STATICTRANS,
	  FLG_UNKNOWNTRANS,
	  FLG_STATICINITTRANS,
	  FLG_UNKNOWNINITTRANS,
	  FLG_KEEPTRANS,
	  FLG_IMMEDIATETRANS,
	  FLG_ONLYUNQGLOBALTRANS,
	  FLG_USERELEASED, FLG_ALIASUNIQUE, FLG_MAYALIASUNIQUE,
	  FLG_MUSTFREE, FLG_MUSTDEFINE, FLG_GLOBSTATE, 
	  FLG_COMPDESTROY, FLG_MUSTNOTALIAS,
	  FLG_MEMIMPLICIT,
	  FLG_BRANCHSTATE, 
	  FLG_NULLPOINTERARITH,
	  FLG_SHADOW, FLG_DEPARRAYS,
	  FLG_REDECL, FLG_READONLYSTRINGS, FLG_READONLYTRANS,
	  FLG_LOOPLOOPBREAK, FLG_SWITCHLOOPBREAK, FLG_MODGLOBS,
	  FLG_CHECKSTRICTGLOBALS, FLG_IMPCHECKEDSPECGLOBALS,
          FLG_MACROMATCHNAME, FLG_WARNLINTCOMMENTS,
	  FLG_INCLUDENEST, FLG_ANSIRESERVED, FLG_CPPNAMES, 
	  FLG_NOPARAMS, FLG_IFEMPTY, FLG_WHILEEMPTY, FLG_REALCOMPARE,
	  FLG_BOOLOPS, FLG_SHIFTSIGNED,
	  INVALID_FLAG } ;

      SETFLAGS ();
    }
  else if (cstring_equalLit (s, "strict"))
    {
      flagcode modeflags[] = 
	{ 
	  FLG_CHECKSTRICTGLOBALIAS,
	  FLG_CHECKEDGLOBALIAS,
	  FLG_CHECKMODGLOBALIAS,
	  FLG_UNCHECKEDGLOBALIAS,
	  FLG_MODFILESYSTEM,
	  FLG_MACROMATCHNAME,
          FLG_MACROUNDEF, FLG_MUTREP, FLG_MUSTMOD,
	  FLG_ALLGLOBALS, FLG_IMPTYPE,
	  FLG_MODNOMODS, FLG_MODGLOBSUNSPEC, FLG_MODSTRICTGLOBSUNSPEC,
	  FLG_GLOBUNSPEC, FLG_SIZEOFTYPE,
	  FLG_EXPORTHEADER, FLG_EXPORTHEADERVAR,
	  FLG_NOPARAMS, FLG_OLDSTYLE, FLG_EXITARG, 
	  FLG_RETSTACK,
	  FLG_FCNDEREF,
	  FLG_ONLYUNQGLOBALTRANS,
	  FLG_GLOBALSIMPMODIFIESNOTHING,
	  FLG_PREDBOOLOTHERS, FLG_PREDBOOLPTR, FLG_PREDBOOLINT,
	  FLG_INTERNALGLOBS, FLG_INTERNALGLOBSNOGLOBS,
	  FLG_USEALLGLOBS, FLG_RETALIAS, 
	  FLG_MODGLOBS, FLG_MODGLOBSUNSPEC, FLG_MODGLOBSUNCHECKED,
	  FLG_RETEXPOSE, FLG_ASSIGNEXPOSE, FLG_CASTEXPOSE,
	  FLG_NOEFFECTUNCON, FLG_EVALORDERUNCON,
	  FLG_FUNCUNUSED,
	  FLG_EXPORTITER, FLG_EXPORTCONST,
	  FLG_TYPEUNUSED, FLG_FIELDUNUSED, FLG_PARAMUNUSED, FLG_TOPUNUSED,
	  FLG_CONSTUNUSED, FLG_ENUMMEMUNUSED,
	  FLG_VARUNUSED, 
	  FLG_NULLPOINTERARITH, FLG_POINTERARITH, 
	  FLG_PTRNUMCOMPARE, 
	  FLG_BOOLCOMPARE, FLG_NOEFFECT, 
	  FLG_RETVALINT, FLG_RETVALBOOL, FLG_RETVALOTHER, 
	  FLG_ANSIRESERVED, FLG_ANSIRESERVEDLOCAL, FLG_CPPNAMES,
	  FLG_RETVALBOOL, FLG_RETVALINT, FLG_SPECUNDEF, 
	  FLG_DECLUNDEF, FLG_STRICTOPS, FLG_INCONDEFS, 
	  FLG_INCONDEFSLIB, FLG_MATCHFIELDS, FLG_EXPORTMACRO, FLG_EXPORTVAR, 
	  FLG_EXPORTFCN, FLG_EXPORTTYPE, FLG_EXPORTLOCAL, FLG_MACROPARAMS, 
	  FLG_MACROASSIGN,
	  FLG_SEFPARAMS, FLG_SEFUNSPEC, FLG_MACROSTMT, FLG_MACROPARENS, 
	  FLG_MACROFCNDECL,
	  FLG_MACROCONSTDECL,
	  FLG_MACROREDEF, FLG_MACROEMPTY,
	  FLG_INFLOOPS, FLG_INFLOOPSUNCON,
	  FLG_UNREACHABLE, 
	  FLG_NORETURN, FLG_CASEBREAK, FLG_MISSCASE, FLG_USEDEF,
	  FLG_EVALORDER,
	  FLG_MODUNCON, FLG_MODUNCONNOMODS, FLG_MODINTERNALSTRICT,
	  FLG_MODOBSERVERUNCON,

	  FLG_NESTEDEXTERN, 
	  FLG_FIRSTCASE,

	  /* memchecks flags */
	  FLG_NULLSTATE, FLG_NULLDEREF, FLG_NULLASSIGN,
	  FLG_NULLPASS, FLG_NULLRET,

	  FLG_COMPDEF, FLG_COMPMEMPASS, FLG_UNIONDEF,

	  /* memtrans flags */
	  FLG_EXPOSETRANS,
	  FLG_OBSERVERTRANS,
	  FLG_DEPENDENTTRANS,
	  FLG_NEWREFTRANS,
	  FLG_ONLYTRANS,
	  FLG_OWNEDTRANS,
	  FLG_FRESHTRANS,
	  FLG_SHAREDTRANS,
	  FLG_TEMPTRANS,
	  FLG_KEPTTRANS,
	  FLG_REFCOUNTTRANS,
	  FLG_STATICTRANS,
	  FLG_UNKNOWNTRANS,
	  FLG_KEEPTRANS,
	  FLG_IMMEDIATETRANS,
	  FLG_STATICINITTRANS,
	  FLG_UNKNOWNINITTRANS,

	  FLG_USERELEASED, FLG_ALIASUNIQUE, FLG_MAYALIASUNIQUE,
	  FLG_MUSTFREE, FLG_MUSTDEFINE, FLG_GLOBSTATE, 
	  FLG_COMPDESTROY, FLG_MUSTNOTALIAS,
	  FLG_MEMIMPLICIT,
	  FLG_BRANCHSTATE, 

	  FLG_DECLPARAMNAME, FLG_DECLPARAMMATCH,

	  FLG_SHADOW, FLG_DEPARRAYS, 
	  FLG_STRICTDESTROY, FLG_STRICTUSERELEASED, FLG_STRICTBRANCHSTATE,
	  FLG_REDECL, FLG_READONLYSTRINGS, FLG_READONLYTRANS,
	  FLG_LOOPLOOPBREAK, FLG_LOOPSWITCHBREAK, FLG_SWITCHLOOPBREAK,
	  FLG_SWITCHSWITCHBREAK, FLG_LOOPLOOPCONTINUE,
	  FLG_CHECKSTRICTGLOBALS, FLG_IMPCHECKEDSPECGLOBALS,
	  FLG_ALLGLOBALS, FLG_IMPCHECKEDSTRICTGLOBALS,
	  FLG_IMPCHECKEDSTRICTSTATICS,
	  FLG_IMPCHECKEDSTRICTSPECGLOBALS,
	  FLG_IMPCHECKMODINTERNALS,
	  FLG_WARNMISSINGGLOBALS, FLG_WARNMISSINGGLOBALSNOGLOBS,
	  FLG_WARNLINTCOMMENTS, FLG_ANSIRESERVEDLOCAL,
	  FLG_INCLUDENEST, FLG_STRINGLITERALLEN,
	  FLG_NUMSTRUCTFIELDS, FLG_NUMENUMMEMBERS,
	  FLG_CONTROLNESTDEPTH,
	  FLG_FORBLOCK, FLG_WHILEBLOCK,
	  FLG_FOREMPTY, FLG_WHILEEMPTY,
	  FLG_IFEMPTY, FLG_IFBLOCK,
	  FLG_ELSEIFCOMPLETE,
	  FLG_REALCOMPARE, FLG_BOOLOPS,
	  FLG_SYSTEMDIRERRORS, FLG_UNUSEDSPECIAL,

	  FLG_SHIFTSIGNED, FLG_BITWISEOPS,
	  INVALID_FLAG } ;

      SETFLAGS ();
    }
  else
    {
      llcontbug (message ("context_setMode: bad mode: %s", s));
     }
}

bool
context_isSpecialFile (cstring fname)
{
  char *ext = filenameExtension (cstring_toCharsSafe (fname));
  
  return (mstring_equal (ext, ".y") 
	  || mstring_equal (ext, ".l")
	  || cstring_equalLit (fname, "lex.yy.c"));
}

bool
context_isSystemDir (cstring dir)
{
  cstring sysDirs = context_exposeString (FLG_SYSTEMDIRS);
  char *thisdir = cstring_toCharsSafe (sysDirs);
  char *nextdir = strchr (thisdir, SEPCHAR);

  if (nextdir != NULL)
    {
      *nextdir = '\0';
      nextdir += 1;
    }

  while (thisdir != NULL)
    {
      DPRINTF (("Test: %s / %s", dir, thisdir));

      if (cstring_equalCanonicalPrefix (dir, thisdir))
	{
	  if (nextdir != NULL)
	    {
	      *(nextdir - 1) = SEPCHAR;
	    }
	  
	  return TRUE;
	}

      if (nextdir != NULL)
	{
	  *(nextdir - 1) = SEPCHAR;
	}

      if (nextdir != NULL)
	{
	  thisdir = nextdir;
	  nextdir = strchr (thisdir, SEPCHAR);
	  
	  if (nextdir != NULL)
	    {
	      *nextdir = '\0';
	      nextdir += 1;
	    } 
	}
      else
	{
	  break;
	}
    } 

  DPRINTF (("Returns FALSE"));
  return FALSE;
}

void
context_addFileAccessType (typeId t)
{
  cstring base;

  if (gc.kind == CX_FUNCTION || gc.kind == CX_MACROFCN 
      || gc.kind == CX_UNKNOWNMACRO)
    {
      gc.acct = typeIdSet_insert (gc.acct, t);
    }
  
  gc.facct = typeIdSet_insert (gc.facct, t);
  
  base = fileloc_getBase (g_currentloc);
  insertModuleAccess (base, t);
  DPRINTF (("Add file access: %s / %s", typeIdSet_unparse (gc.facct),
	    typeIdSet_unparse (gc.acct)));
}

void
context_removeFileAccessType (typeId t)
{
  if (gc.kind == CX_FUNCTION || gc.kind == CX_MACROFCN 
      || gc.kind == CX_UNKNOWNMACRO)
    {
      gc.acct = typeIdSet_removeFresh (gc.acct, t);
    }
  
  gc.facct = typeIdSet_removeFresh (gc.facct, t);
  gc.nacct = typeIdSet_insert (gc.nacct, t);
}

void context_enterFunctionDecl (void)
{
  llassert (gc.kind == CX_GLOBAL);
  gc.kind = CX_FCNDECL;
}

void context_exitFunctionDecl (void)
{
  gc.kind = CX_GLOBAL;
}

bool context_inFunctionDecl (void)
{
  return (gc.kind == CX_FCNDECL);
}

void
context_enterMacro (/*@observer@*/ uentry e)
{
  context_enterFunction (e);
  gc.kind = CX_MACROFCN;
}

void
context_enterUnknownMacro (/*@dependent@*/ uentry e)
{
  llassert (uentry_isFunction (e));
  context_enterFunction (e);
  gc.kind = CX_UNKNOWNMACRO;
}

void context_enterAndClause (exprNode e)
{
  
  usymtab_trueBranch (guardSet_copy (exprNode_getGuards (e)));
  pushClause (ANDCLAUSE);
}

void context_enterOrClause (exprNode e)
{
  usymtab_trueBranch (guardSet_invert (exprNode_getGuards (e)));
  pushClause (ORCLAUSE);
}

bool context_inDeepLoop (void)
{
  bool inLoop = FALSE;

  clauseStack_elements (gc.clauses, el)
    {
      if (clause_isLoop (el))
	{
	  if (inLoop)
	    {
	      return TRUE;
	    }

	  inLoop = TRUE;
	}
    } end_clauseStack_elements;

  return FALSE;
}

bool context_inDeepSwitch (void)
{
  bool inLoop = FALSE;

  clauseStack_elements (gc.clauses, el)
    {
      if (clause_isSwitch (el))
	{
	  if (inLoop)
	    {
	      return TRUE;
	    }

	  inLoop = TRUE;
	}
    } end_clauseStack_elements;

  return FALSE;
}

bool context_inDeepLoopSwitch (void)
{
  bool inLoop = FALSE;

  clauseStack_elements (gc.clauses, el)
    {
      if (clause_isBreakable (el))
	{
	  if (inLoop)
	    {
	      return TRUE;
	    }

	  inLoop = TRUE;
	}
    } end_clauseStack_elements;

  return FALSE;
}

clause context_breakClause (void)
{
  clauseStack_elements (gc.clauses, el)
    {
      if (clause_isSwitch (el))
	{
	  return el;
	}
      else if (clause_isLoop (el))
	{
	  return el;
	}
      else
	{
	  ;
	}
    } end_clauseStack_elements;

  return NOCLAUSE;
}

clause context_nextBreakClause (void)
{
  bool hasOne = FALSE;

  clauseStack_elements (gc.clauses, el)
    {
      if (clause_isBreakable (el))
	{
	  if (hasOne)
	    {
	      return el;
	    }
	  else
	    {
	      hasOne = TRUE;
	    }
	}
    } end_clauseStack_elements;

  return NOCLAUSE;
}
  
bool context_inConditional (void)
{
  clauseStack_elements (gc.clauses, el)
    {
      /*
      ** Should also include TRUECLAUSE and FALSECLAUSE, but need
      ** to distinguish if from ? for this
      */

      if (clause_isBreakable (el) && (el != DOWHILECLAUSE))
	{
	  return TRUE;
	}
    } end_clauseStack_elements;

  return FALSE;
}

void context_exitAndClause (exprNode pred, exprNode tbranch)
{
  context_setJustPopped ();
  
  llassert (gc.inclause == ANDCLAUSE);
  
  usymtab_popAndBranch (pred, tbranch);
  clauseStack_pop (gc.clauses);
  gc.inclause = topClause (gc.clauses);
}

void context_exitOrClause (exprNode pred, exprNode tbranch)
{
  context_setJustPopped ();
  
  llassert (gc.inclause == ORCLAUSE);
  
  usymtab_popOrBranch (pred, tbranch);
  clauseStack_pop (gc.clauses);
  gc.inclause = topClause (gc.clauses);
}

static void context_enterCondClauseAux (clause cl)
     /*@modifies gc@*/
{
  pushClause (cl);
}

static void context_enterTrueAux (exprNode e, clause cl)
   /*@modifies gc@*/
{
  usymtab_trueBranch (guardSet_copy (exprNode_getGuards (e)));
  pushClause (cl);
}

void context_enterIterClause (void)
{
  context_enterTrueAux (exprNode_undefined, ITERCLAUSE);
}

void context_enterDoWhileClause (void)
{
  pushClause (DOWHILECLAUSE);
}

void context_enterWhileClause (exprNode e)
{
  context_enterTrueAux (e, WHILECLAUSE);
}

void context_enterForClause (exprNode e)
{
  context_enterTrueAux (e, FORCLAUSE);
}

void context_enterTrueClause (exprNode e)
{
  context_enterTrueAux (e, TRUECLAUSE);
}

void context_enterSwitch (exprNode e)
{
  
  usymtab_switchBranch (e);
  context_enterCondClauseAux (SWITCHCLAUSE);
}

void context_exitSwitch (exprNode e, bool allpaths)
{
    usymtab_exitSwitch (e, allpaths);
  
  while (clause_isCase (clauseStack_top (gc.clauses)))
    {
      clauseStack_pop (gc.clauses);
    }
  
  context_exitClauseSimp ();
}

void context_enterCaseClause (exprNode e)
{
  bool branch = FALSE;
 
  DPRINTF (("Enter case clause!"));

  branch = usymtab_newCase (exprNode_undefined, e);
  
  if (branch)
    {
      context_enterCondClauseAux (CASECLAUSE);
    }
}

static void context_enterFalseClauseAux (exprNode e, clause cl)
     /*@modifies gc@*/
{
  usymtab_altBranch (guardSet_invert (exprNode_getGuards (e)));
  gc.inclause = cl;
  clauseStack_switchTop (gc.clauses, cl);
}

void context_enterFalseClause (exprNode e)
{
  
  context_enterFalseClauseAux (e, FALSECLAUSE);
}

void
context_enterConstantMacro (/*@exposed@*/ /*@dependent@*/ uentry e)
{
  gc.kind = CX_MACROCONST;
  gc.cont.fcn = e;
  gc.showfunction = context_getFlag (FLG_SHOWFUNC);

  gc.acct = typeIdSet_subtract (typeIdSet_union (gc.facct, uentry_accessType (e)), 
				gc.nacct);

  
  usymtab_enterScope ();
  sRef_enterFunctionScope ();

  gc.globs = globSet_undefined;
  globSet_clear (gc.globs_used);
  gc.mods = sRefSet_undefined;
}

uentry context_getHeader (void)
{
  if (!(context_inFunctionLike () || (gc.kind == CX_MACROCONST)))
    {
      llfatalbug (message ("context_getHeader: bad call: %q",
			   context_unparse ()));
    }

  return (gc.cont.fcn);
}

void
context_setFunctionDefined (fileloc loc)
{
  switch (gc.kind)
    {
    case CX_UNKNOWNMACRO:
    case CX_FUNCTION:
    case CX_MACROFCN:
      uentry_setFunctionDefined (gc.cont.fcn, loc);
      break;
    default:
      /* (not a bug because of parse errors) */
      break;
    }
}

void
context_enterFunction (/*@exposed@*/ uentry e)
{
  gc.kind = CX_FUNCTION;
  gc.cont.fcn = e;

  if (uentry_hasAccessType (e))
    {
      gc.acct = typeIdSet_subtract (typeIdSet_union (gc.facct, uentry_accessType (e)), 
				    gc.nacct);
    }
  else
    {
      gc.acct = gc.facct;
    }

  DPRINTF (("Enter function: %s / %s", uentry_unparse (e), 
	    typeIdSet_unparse (gc.acct)));

  gc.showfunction = context_getFlag (FLG_SHOWFUNC);
  
  gc.globs = uentry_getGlobs (e);
  globSet_clear (gc.globs_used);
  gc.mods = uentry_getMods (e);

  usymtab_enterFunctionScope (e);
  sRef_enterFunctionScope ();
}

static bool context_checkStrictGlobals (void)
{
  return (context_getFlag (FLG_CHECKSTRICTGLOBALS));
}

static bool context_hasGlobs (void)
{
  if (context_inFunctionLike ())
    {
      return (uentry_hasGlobs (gc.cont.fcn));
    }
  else
    {
      return (FALSE);
    }
}

static bool context_checkCheckedGlobals (void)
{
  return (context_getFlag (FLG_GLOBALS)
	  && (context_getFlag (FLG_GLOBUNSPEC)
	      || context_hasGlobs ()));
}

static bool context_checkUnknownGlobals (void)
{
  /* should be uentry_hasGlobs ? */

  return (context_getFlag (FLG_ALLGLOBALS)
	  && (context_getFlag (FLG_GLOBUNSPEC)
	      || context_hasGlobs ()));
}      

static bool context_checkUncheckedGlobals (void)
{
  return (FALSE);
}      

bool 
context_checkExport (uentry e)
{
  if (!gc.anyExports) return FALSE;

  if (uentry_isFunction (e)
      || (uentry_isVariable (e) && ctype_isFunction (uentry_getType (e))))
    {
      return context_maybeSet (FLG_EXPORTFCN);
    }
  else if (uentry_isExpandedMacro (e))
    {
      return context_maybeSet (FLG_EXPORTMACRO);
    }
  else if (uentry_isVariable (e))
    {
      return context_maybeSet (FLG_EXPORTVAR);
    }
  else if (uentry_isEitherConstant (e))
    {
      return context_maybeSet (FLG_EXPORTCONST);
    }
  else if (uentry_isIter (e) || uentry_isEndIter (e))
    {
      return context_maybeSet (FLG_EXPORTITER);
    }
  else if (uentry_isDatatype (e))
    {
      return context_maybeSet (FLG_EXPORTTYPE);
    }
  else
    {
      BADEXIT;
    }
}
	      
bool
context_checkGlobUse (uentry glob)
{
  
  if (uentry_isCheckedStrict (glob))
    {
      return context_checkStrictGlobals ();
    }
  else if (uentry_isChecked (glob))
    {
      return context_checkCheckedGlobals ();
    }
  else if (uentry_isCheckedUnknown (glob) || uentry_isCheckMod (glob))
    {
      return context_checkUnknownGlobals ();
    }
  else 
    {
      llassert (uentry_isUnchecked (glob));

      return context_checkUncheckedGlobals ();
    }
}

bool
context_checkAliasGlob (uentry glob)
{
  if (uentry_isCheckedStrict (glob))
    {
      return gc.flags[FLG_CHECKSTRICTGLOBALIAS];
    }
  else if (uentry_isChecked (glob))
    {
      return gc.flags[FLG_CHECKEDGLOBALIAS];
    }
  else if (uentry_isCheckMod (glob))
    {
      return gc.flags[FLG_CHECKMODGLOBALIAS];
    }
  else 
    {
      llassert (uentry_isUnchecked (glob) || uentry_isCheckedUnknown (glob));

      return gc.flags[FLG_UNCHECKEDGLOBALIAS];
    }
}

bool context_checkInternalUse (void)
{
  if (context_hasGlobs ())
    {
      return (gc.flags[FLG_INTERNALGLOBS]);
    }
  else
    {
      return (gc.flags[FLG_INTERNALGLOBSNOGLOBS]);
    }
}

bool
context_checkGlobMod (sRef el)
{
  uentry ue = sRef_getUentry (el);

  /* no: llassert (sRef_isGlobal (el)); also check local statics */

  if (uentry_isCheckedModify (ue)
      || (!uentry_isUnchecked (ue) && (gc.flags[FLG_ALLGLOBALS])))
    {
      if (context_hasMods ())
	{
	  return (gc.flags[FLG_MODGLOBS]);
	}
      else
	{
	  if (uentry_isCheckedStrict (ue))
	    {
	      return (gc.flags[FLG_MODGLOBSUNSPEC]);
	    }
	  else
	    {
	      return (gc.flags[FLG_MODSTRICTGLOBSUNSPEC]);
	    }
	}
    }
  else
    {
      if (context_hasMods ())
	{
	  return (gc.flags[FLG_MODGLOBSUNCHECKED]);
	}
      else
	{
	  return FALSE;
	}
    }
}

void
context_usedGlobal (sRef el)
{
  if (!globSet_member (gc.globs_used, el))
    {
      /* 
      ** The first time a global is used in a function, we need
      ** to clear the derived sRefs, since they were set for the
      ** previous function.
      */

      sRef_clearDerived (el);
      gc.globs_used = globSet_insert (gc.globs_used, el);
    }
}

/*@observer@*/ sRefSet
context_modList (void)
{
  return gc.mods;
}

bool
context_globAccess (sRef s)
{
  llassert (sRef_isGlobal (s) || sRef_isKindSpecial (s));
  return (globSet_member (gc.globs, s));
}

bool
context_hasAccess (typeId t)
{
  if (context_inFunctionLike ())
    {
      return (typeIdSet_member (gc.acct, t));
    }
  else
    {
      return (context_hasFileAccess (t));
    }
}

bool
context_hasFileAccess (typeId t)
{
  return (typeIdSet_member (gc.facct, t));
}

/*@only@*/ cstring
context_unparseAccess (void)
{
  return (message ("%q / %q", typeIdSet_unparse (gc.acct),
		   typeIdSet_unparse (gc.facct)));
}

/*@only@*/ cstring
context_unparseClauses (void)
{
  return (clauseStack_unparse (gc.clauses));
}

bool
context_couldHaveAccess (typeId t)
{
  if (gc.kind == CX_FUNCTION || gc.kind == CX_MACROFCN || gc.kind == CX_UNKNOWNMACRO)
    {
      return (typeIdSet_member (gc.acct, t));
    }
  else
    {
      return (typeIdSet_member (gc.facct, t)); 
    }
}

ctype
context_getRetType (void)
{
  ctype f = ctype_undefined;

  if (gc.kind == CX_FUNCTION || gc.kind == CX_MACROFCN)
    {
      f = uentry_getType (gc.cont.fcn);
    }
  else if (gc.kind == CX_UNKNOWNMACRO)
    {
      return ctype_unknown;
    }
  else
    {
      llcontbuglit ("context_getRetType: not in a function context");
      return ctype_unknown;
    }

  if (!ctype_isFunction (f))
    {
      if (ctype_isKnown (f))
	{
	  llbuglit ("context_getRetType: not a function");
	}

      return ctype_unknown;
    }
  return (ctype_returnValue (f));
}    

bool
context_hasMods (void)
{
  if (context_inFunctionLike ())
    {
      return (uentry_hasMods (gc.cont.fcn));
    }
  else
    {
      return FALSE;
    }
}

void
context_exitAllClauses (void)
{
  
  while (!clauseStack_isEmpty (gc.clauses))
    {
      clause el = clauseStack_top (gc.clauses);

      gc.inclause = el;

      if (clause_isNone (el))
	{
	  usymtab_quietExitScope (g_currentloc);
	  clauseStack_pop (gc.clauses);
	}
      else
	{
	  context_exitClausePlain ();
	}
    }

  clauseStack_clear (gc.clauses);

  
  gc.inclause = NOCLAUSE;
}

static
void context_exitClauseSimp (void)
{
  
  context_setJustPopped ();
  clauseStack_pop (gc.clauses);
  gc.inclause = topClause (gc.clauses);
}

static
void context_exitCaseClause (void)
{
  context_setJustPopped ();
  usymtab_popCaseBranch ();
  clauseStack_pop (gc.clauses);
  gc.inclause = topClause (gc.clauses);
}

static
void context_exitClauseAux (exprNode pred, exprNode tbranch)
{
  context_setJustPopped ();
  usymtab_popTrueBranch (pred, tbranch, gc.inclause);
  clauseStack_pop (gc.clauses);
  gc.inclause = topClause (gc.clauses);
}

void context_exitTrueClause (exprNode pred, exprNode tbranch)
{
  if (gc.inclause != TRUECLAUSE)
    {
      llparseerror (cstring_makeLiteral
		    ("Likely parse error.  Conditional clauses are inconsistent."));
      return;
    }

    
  context_setJustPopped ();

  usymtab_popTrueBranch (pred, tbranch, TRUECLAUSE);
  clauseStack_pop (gc.clauses);
  gc.inclause = topClause (gc.clauses);
  
  }

void context_exitIterClause (exprNode body)
{
  llassert (gc.inclause == ITERCLAUSE);

  context_setJustPopped ();

  if (context_getFlag (FLG_LOOPEXEC))
    {
      usymtab_popTrueExecBranch (exprNode_undefined, body, ITERCLAUSE);
    }
  else
    {
      usymtab_popTrueBranch (exprNode_undefined, body, ITERCLAUSE);
    }

  clauseStack_pop (gc.clauses);
  gc.inclause = topClause (gc.clauses);
}

static void context_popCase (void) {
  /*
  ** If we are exiting an outer clause, sometimes still in a switch case.
  **
  ** e.g.: 
  **
  ** switch(a)
  ** {
  **   case 1:
  **     while (c>3)
  **       {
  **         case 3: ++c;
  **       }
  ** }     
  */

  DPRINTF (("Popping case clause: %s",
	    clauseStack_unparse (gc.clauses)));

  if (gc.inclause == CASECLAUSE) {
    context_exitCaseClause ();
  }
}

void context_exitWhileClause (exprNode pred, exprNode body)
{
  guardSet invGuards = guardSet_invert (exprNode_getGuards (pred));

  context_popCase (); 

  if (gc.inclause != WHILECLAUSE) {
    DPRINTF (("Clause: %s / %s", clause_unparse (gc.inclause),
	      clauseStack_unparse (gc.clauses)));
  }

  llassert (gc.inclause == WHILECLAUSE);

  context_setJustPopped ();

  
  /*
  ** predicate must be false after while loop (unless there are breaks)
  */

  if (context_getFlag (FLG_LOOPEXEC))
    {
      usymtab_popTrueExecBranch (pred, body, WHILECLAUSE);
    }
  else
    {
      usymtab_popTrueBranch (pred, body, WHILECLAUSE);
    }

  
  usymtab_addGuards (invGuards);
  guardSet_free (invGuards);

  clauseStack_pop (gc.clauses);
  gc.inclause = topClause (gc.clauses);  
}

void context_exitDoWhileClause (exprNode pred)
{
  guardSet invGuards = guardSet_invert (exprNode_getGuards (pred));

  if (gc.inclause == CASECLAUSE) {
    /* handle Duff's device */
    clauseStack_pop (gc.clauses);
    gc.inclause = topClause (gc.clauses);
  }

  llassert (gc.inclause == DOWHILECLAUSE);

  context_setJustPopped ();

    
  usymtab_addGuards (invGuards);
  guardSet_free (invGuards);

  clauseStack_pop (gc.clauses);
  gc.inclause = topClause (gc.clauses);  
}

void context_exitForClause (exprNode forPred, exprNode body)
{
  guardSet invGuards = guardSet_invert (exprNode_getForGuards (forPred));

  llassert (gc.inclause == FORCLAUSE);
  context_setJustPopped ();

  /*
  ** predicate must be false after while loop (unless there are breaks)
  */

  if (context_getFlag (FLG_LOOPEXEC))
    {
            usymtab_popTrueExecBranch (forPred, body, FORCLAUSE);
    }
  else
    {
      usymtab_popTrueBranch (forPred, body, FORCLAUSE);
    }

  usymtab_addGuards (invGuards);
  guardSet_free (invGuards);
  clauseStack_pop (gc.clauses);
  gc.inclause = topClause (gc.clauses);
}

static void context_exitClausePlain (void)
{
  llassert (gc.inclause != NOCLAUSE);
  
  if (gc.inclause == FALSECLAUSE)
    {
      context_exitClause (exprNode_undefined, exprNode_undefined, exprNode_undefined);
    }
  else
    {
      context_exitClauseAux (exprNode_undefined, exprNode_undefined);
    }
  
}

void context_exitClause (exprNode pred, exprNode tbranch, exprNode fbranch)
{
    
  context_setJustPopped ();

  if (gc.inclause == FALSECLAUSE)
    {
      usymtab_popBranches (pred, tbranch, fbranch, FALSE, FALSECLAUSE);

      llassert (clauseStack_top (gc.clauses) == FALSECLAUSE);

      clauseStack_pop (gc.clauses);
      gc.inclause = topClause (gc.clauses);
    }
  else
    {
            context_exitTrueClause (pred, tbranch);
    }
}

void
context_returnFunction (void)
{
  usymtab_checkFinalScope (TRUE);
}

void
context_exitFunction (void)
{    
  if (!context_inFunction () && !context_inMacroConstant () 
      && !context_inMacroUnknown () 
      && !context_inIterDef () && !context_inIterEnd ())
    {
      /*
      ** not a bug because of parse errors
      */
    }
  else
    {
      if (context_inMacro () && usymtab_inFunctionScope ())
	{
	  usymtab_exitScope (exprNode_undefined);
	}
      
      if (uentry_hasGlobs (gc.cont.fcn))
	{
	  exprChecks_checkUsedGlobs (gc.globs, gc.globs_used);
	}
      
            
      if (uentry_hasMods (gc.cont.fcn))
	{
	  if (context_getFlag (FLG_MUSTMOD))
	    {
	      exprNode_checkAllMods (gc.mods, gc.cont.fcn);
	    }
	}

      /*
      ** clear file static modifies
      */
      
      /* do this first to get unused error messages */

      usymtab_exitScope (exprNode_undefined);
      sRef_exitFunctionScope ();
      
      gc.showfunction = FALSE;
      gc.kind = CX_GLOBAL;
      gc.cont.glob = TRUE;
      gc.acct = gc.facct; 
      gc.globs = globSet_new ();
      globSet_clear (gc.globs_used);
      gc.mods = sRefSet_new ();
    }

  llassert (clauseStack_isEmpty (gc.clauses));
  llassert (gc.inclause == NOCLAUSE);
}

void
context_quietExitFunction (void)
{
  while (gc.kind == CX_INNER)
    { 
      context_exitInnerPlain ();
    }

  if (!context_inFunction () && !context_inMacroConstant () && !context_inMacroUnknown () 
      && !context_inIterDef () && !context_inIterEnd ())
    {
    }
  else
    {
      usymtab_quietExitScope (g_currentloc);

      gc.showfunction = FALSE;
      gc.kind = CX_GLOBAL;
      gc.cont.glob = TRUE;
      gc.acct = gc.facct; 
      gc.globs = globSet_new ();
      globSet_clear (gc.globs_used);
      gc.mods = sRefSet_new ();

      sRef_exitFunctionScope ();
    }
}

/*@observer@*/ uentryList
context_getParams (void)
{
  if (context_inFunctionLike ())
    {
      return (uentry_getParams (gc.cont.fcn));
    }
  else
    {
      llcontbug (message ("context_getParams: not in function: %q", context_unparse ()));
      return uentryList_undefined;
    }
}

/*@observer@*/ globSet
context_getUsedGlobs (void)
{
  llassert (gc.kind == CX_FUNCTION || gc.kind == CX_MACROFCN 
	    || gc.kind == CX_UNKNOWNMACRO || gc.kind == CX_ITERDEF);

  return (gc.globs_used);
}

cstring
context_moduleName ()
{
  return (fileloc_getBase (g_currentloc));
}

/*@observer@*/ globSet
context_getGlobs (void)
{
  llassert (gc.kind == CX_FUNCTION || gc.kind == CX_MACROFCN 
	    || gc.kind == CX_UNKNOWNMACRO || gc.kind == CX_ITERDEF);

  return (gc.globs);
}

void
context_addBoolAccess (void)
{
  cstring bname = context_getString (FLG_BOOLTYPE);
  typeIdSet boolt = typeIdSet_single (usymtab_getTypeId (bname));
  
  addModuleAccess (cstring_copy (bname), boolt);

  /* for sys/types (perhaps, this is bogus!) */ 
  addModuleAccess (cstring_makeLiteral ("types"), boolt); 
}

# if 0
bool
context_canAccessBool (void)
{
  return TRUE;
}
# endif

/*
  static typeId boolType = typeId_invalid;

  if (typeId_isInvalid (boolType))
    { 
      boolType = usymtab_getTypeId (context_getBoolName ());
    }

  if (typeId_isInvalid (boolType)) {
    return FALSE;
  } else {
    return (typeIdSet_member (gc.acct, boolType));
  }
}
*/

/* evs 2000-07-25: old version - replaced */

ctype
context_boolImplementationType () {
  /* For now, this is bogus! */
  return ctype_int;
}

bool
context_canAccessBool (void)
{
  static typeId boolType = typeId_invalid;

  if (typeId_isInvalid (boolType))
    { 
      boolType = usymtab_getTypeId (context_getBoolName ());
    }

  if (!typeId_isInvalid (boolType))
    { 
      return context_hasAccess (boolType);
    }
  else 
    {
      ;
    }

  return FALSE; 
}

void
context_setMessageAnnote (/*@only@*/ cstring s)
{
  llassert (cstring_isUndefined (gc.msgAnnote));
    gc.msgAnnote = s;
}

bool
context_hasMessageAnnote (void)
{
  return (cstring_isDefined (gc.msgAnnote));
}

void
context_clearMessageAnnote (void)
{
  if (cstring_isDefined (gc.msgAnnote))
    {
      cstring_free (gc.msgAnnote);
      gc.msgAnnote = cstring_undefined;
    }
}

/*@only@*/ cstring
context_getMessageAnnote (void)
{
  cstring st = gc.msgAnnote;

    gc.msgAnnote = cstring_undefined;
  return st;
}

void
context_setAliasAnnote (/*@observer@*/ sRef s, /*@observer@*/ sRef t)
{
    llassert (sRef_isInvalid (gc.aliasAnnote));
  llassert (!sRef_sameName (s, t));
  gc.aliasAnnote = s;
  gc.aliasAnnoteAls = t;
}

bool
context_hasAliasAnnote (void)
{
  return (sRef_isValid (gc.aliasAnnote));
}

void
context_clearAliasAnnote (void)
{
  gc.aliasAnnote = sRef_undefined;
}

cstring
context_getAliasAnnote (void)
{
  sRef ret = gc.aliasAnnote;
  sRef als = gc.aliasAnnoteAls;

  llassert (sRef_isValid (ret) && sRef_isValid (als));

  gc.aliasAnnote = sRef_undefined;
  return (message ("%q aliases %q", sRef_unparse (als), sRef_unparse (ret)));
}

void
context_recordFileModifies (sRefSet mods)
{
    gc.modrecs = sRefSetList_add (gc.modrecs, mods);
}

void
context_recordFileGlobals (globSet mods)
{
    /*@access globSet@*/ context_recordFileModifies (mods); /*@noaccess globSet@*/
}

void
context_setCommentMarkerChar (char c)
{
  llassert (c != '\0');

  context_setValue (FLG_COMMENTCHAR, (int) c);
}

char
context_getCommentMarkerChar (void)
{
  return ((char) context_getValue (FLG_COMMENTCHAR));
}

static void
context_setValue (flagcode flag, int val)
{
  int index = flagcode_valueIndex (flag);

  llassert (index >= 0 && index <= NUMVALUEFLAGS);

  if (val <= 0)
    {
      switch (flag)
	{
	case FLG_INCLUDENEST:
	case FLG_CONTROLNESTDEPTH:
	case FLG_STRINGLITERALLEN:
	case FLG_NUMSTRUCTFIELDS:
	case FLG_NUMENUMMEMBERS:
	case FLG_LINELEN:
	  {
	    cstring warn = message ("Value for %s must be a positive "
				    "number (given %d)",
				    flagcode_unparse (flag), val);
	    
	    flagWarning (warn);
	    cstring_free (warn);
	    val = MINLINELEN;
	  }
	  return;
	default:
	  break;
	}
    }

  if (flag == FLG_LINELEN && val < MINLINELEN)
    {
      cstring warn = message ("Value for %s must be at least %d (given %d)",
			      flagcode_unparse (flag), 
			      MINLINELEN, val);
      flagWarning (warn);
      cstring_free (warn);
      val = MINLINELEN;
    }

    gc.values[index] = val;
}

void
context_setValueAndFlag (flagcode flag, int val)
{
  gc.flags[flag] = TRUE;
  context_setValue (flag, val);
}

int
context_getValue (flagcode flag)
{
  int index = flagcode_valueIndex (flag);

  llassert (index >= 0 && index <= NUMVALUEFLAGS);
  return (gc.values[index]);
}

int
context_getCounter (flagcode flag)
{
  int index = flagcode_valueIndex (flag);

  llassert (index >= 0 && index <= NUMVALUEFLAGS);
  return (gc.counters[index]);
}

void
context_incCounter (flagcode flag)
{
  int index = flagcode_valueIndex (flag);

  llassert (index >= 0 && index <= NUMVALUEFLAGS);
  /* check limit */
  gc.counters[index]++;
}

void
context_decCounter (flagcode flag)
{
  int index = flagcode_valueIndex (flag);

  llassert (index >= 0 && index <= NUMVALUEFLAGS);
  gc.counters[index]--;
}

bool context_showFunction (void)
{
  return (gc.showfunction);
}

void
context_setString (flagcode flag, cstring val)
{
  int index = flagcode_stringIndex (flag);

  llassert (index >= 0 && index <= NUMSTRINGFLAGS);

  if (flag == FLG_SYSTEMDIRS)
    {
      llassert (cstring_isDefined (val));

      if (cstring_firstChar (val) == '\"')
	{
	  cstring oval = val;
	  cstring tval = cstring_copy (cstring_suffix (val, 1));
	
	  if (cstring_lastChar (tval) != '\"')
	    {
	      int n = cstring_length (tval) - 1;

	      while (isspace ((int) cstring_getChar (tval, n)))
		{
		  n--;
		}

	      if (cstring_getChar (tval, n) != '\"')
		{
		  cstring msg = message ("Setting -systemdirs to string with unmatching quotes: %s", val);
		  flagWarning (msg);
		  cstring_free (msg);
		}
	      else
		{
		  cstring otval = tval;
		  tval = cstring_prefix (tval, n);
		  cstring_free (otval);
		}
	    }
	  
	  val = cstring_copy (cstring_clip (tval, cstring_length (tval) - 1));
	  DPRINTF (("val = %s", val));
	  cstring_free (tval);
	  cstring_free (oval);
	}
    }

  if (flag == FLG_TMPDIR)
    {
      llassert (cstring_isDefined (val));
      
      if (cstring_length (val) == 0)
	{
	  cstring_free (val);
	  val = message (".%s", cstring_makeLiteralTemp (CONNECTSTR));
	}
      else if (cstring_lastChar (val) != CONNECTCHAR)
	{
	  val = cstring_appendChar (val, CONNECTCHAR);
	}
      else
	{
	  ;
	}
    }

  if (cstring_length (val) >= 1
      && cstring_firstChar (val) == '"')
    {
      cstring s = message
		   ("setting %s to string beginning with \".  You probably "
		    "don't meant to have the \"'s.",
		    flagcode_unparse (flag));

      flagWarning (s);
      cstring_free (s);
    }

  if (flag == FLG_BOOLTYPE)
    {

    }

  gc.strings[index] = val;
}

static /*@exposed@*/ cstring
context_exposeString (flagcode flag)
{
  int index = flagcode_stringIndex (flag);

  llassert (index >= 0 && index <= NUMSTRINGFLAGS);
  return (gc.strings[index]);
}

cstring
context_getString (flagcode flag)
{
  return (context_exposeString (flag));
}

void
context_resetErrors (void)
{
  gc.numerrors = 0;
}

void context_initMod (void)
   /*@globals undef gc; @*/
{
  gc.kind = CX_GLOBAL;
  gc.instandardlib = FALSE;
  gc.numerrors = 0;
  gc.neednl = FALSE;
  gc.linesprocessed = 0;
  gc.speclinesprocessed = 0;
  gc.insuppressregion = FALSE;
  gc.macroMissingParams = FALSE;
  gc.preprocessing = FALSE;
  gc.incommandline = FALSE;
  gc.mc = macrocache_create ();
  gc.nmods = 0;
  gc.maxmods = DEFAULTMAXMODS;
  gc.moduleaccess = (maccesst *) 
    dmalloc (sizeof (*gc.moduleaccess) * (gc.maxmods));

  gc.library = FLG_ANSILIB;

  gc.locstack = filelocStack_new ();
  gc.modrecs = sRefSetList_undefined;
  gc.anyExports = FALSE;

  gc.ftab = fileTable_create ();
  gc.msgLog = messageLog_new ();
  gc.inimport = FALSE;
  gc.inDerivedFile = FALSE;
  gc.inheader = FALSE;
  gc.markers = flagMarkerList_new ();
  gc.cont.glob = TRUE;
  gc.showfunction = FALSE;
  gc.msgAnnote = cstring_undefined;
  gc.aliasAnnote = sRef_undefined;
  gc.aliasAnnoteAls = sRef_undefined;
  gc.boolType = ctype_bool;
  gc.mods = sRefSet_new ();

  gc.saveloc = fileloc_undefined;

  gc.inmacrocache = FALSE;
  gc.inclause = NOCLAUSE;
  gc.clauses = clauseStack_new ();
  gc.globs = globSet_new ();
  gc.nacct = typeIdSet_emptySet ();
  gc.acct = typeIdSet_emptySet ();
  gc.facct = typeIdSet_emptySet ();
  gc.savedFlags = FALSE;
  gc.pushloc = fileloc_undefined;
  gc.protectVars = FALSE;
  gc.justpopped = FALSE;
  gc.isNullGuarded = NO;
  gc.globs_used = globSet_undefined;
  
  allFlagCodes (code)
    {
      gc.setGlobally[code] = FALSE;
      gc.setLocally[code] = FALSE;
    } end_allFlagCodes ;

  usymtab_initMod ();

  context_resetAllFlags ();
  conext_resetAllCounters ();
  context_setMode (DEFAULT_MODE);
}

ctype
context_typeofZero (void)
{
  ctype ct = ctype_int;

  if (context_getFlag (FLG_ZEROPTR))
    {
      ct = ctype_makeConj (ct, ctype_voidPointer);
    }
  
  if (context_getFlag (FLG_ZEROBOOL)) {
    ct = ctype_makeConj (ct, ctype_bool); 
  }

  return ct;
}

ctype
context_typeofOne (void)
{
  ctype ct = ctype_int;

  /* 1 is on longer a bool (was before 2.4)
     if (!context_getFlag (FLG_ABSTRACTBOOL))
     {
     ct = ctype_makeConj (ct, ctype_bool);
     }
     */

  return (ct);
}

/*@only@*/ cstring
context_unparse (void)
{
  cstring s;

  switch (gc.kind)
    {
    case CX_LCL:
      s = message ("LCL File: %q", fileloc_unparse (g_currentloc));
      break;
    case CX_LCLLIB:
      s = message ("LCL Lib File: %q", fileloc_unparse (g_currentloc));
      break;
    case CX_GLOBAL:
      s = message ("Global Context:%q", fileloc_unparse (g_currentloc));
      break;
    case CX_INNER:
      s = message ("Inner Context:%q", fileloc_unparse (g_currentloc));
      break;
    case CX_FUNCTION:
      s = message ("Function %q :%q \n\taccess %q\n\tmodifies %q",
		   uentry_unparse (gc.cont.fcn),
		   fileloc_unparse (g_currentloc),
		   typeIdSet_unparse (gc.acct),
		   sRefSet_unparse (gc.mods));
      break;
    case CX_MACROFCN:
      s = message ("Function Macro %q", uentry_unparse (gc.cont.fcn));
      break;
    case CX_UNKNOWNMACRO:
      s = message ("Forward Specified Macro %q", uentry_unparse (gc.cont.fcn));
      break;
    case CX_MACROCONST:
      s = message ("Constant Macro %q", uentry_unparse (gc.cont.fcn));
      break;
    case CX_ITERDEF:
      s = message ("Iter definition %q", uentry_unparse (gc.cont.fcn));
      break;
    case CX_ITEREND:
      s = message ("Iter end %q", uentry_unparse (gc.cont.fcn));
      break;
    default:
      s = message ("Un-unparseable context: %d", (int) gc.kind);
      break;
    }
  
  s = message ("%q\naccess: %q", s, context_unparseAccess ());
  return (s);
}

extern ctype
context_currentFunctionType (void)
{
  if (gc.kind == CX_FUNCTION || gc.kind == CX_MACROFCN)
    {
            return (uentry_getType (gc.cont.fcn));
    }
  else if (gc.kind == CX_INNER)
    {
      llcontbuglit ("context_currentFunctionType: inner context");
      do { context_exitInnerPlain (); } while (gc.kind == CX_INNER);
      return (context_currentFunctionType ());
    }
  else
    {
      llcontbuglit ("context_currentFunctionType: not in function");
      return (ctype_undefined);
    }
}

void
context_enterInnerContext (void)
{
  if (gc.kind == CX_GLOBAL)
    {
      gc.kind = CX_INNER;
      gc.cont.cdepth = 1;
    }
  else if (gc.kind == CX_INNER)
    {
      gc.cont.cdepth++;
    }
  else
    {
      ;
    }

  
  usymtab_enterScope ();
  pushClause (NOCLAUSE);
}

void
context_exitInnerPlain (void) /*@modifies gc;@*/
{
  context_exitInner (exprNode_undefined);
}

void
context_exitInner (exprNode exp)
{
  
  llassertprint (gc.inclause == NOCLAUSE || gc.inclause == CASECLAUSE,
		 ("inclause = %s", clause_nameTaken (gc.inclause)));

  clauseStack_removeFirst (gc.clauses, NOCLAUSE);
  gc.inclause = topClause (gc.clauses);

  if (gc.kind == CX_INNER)
    {
      if (--gc.cont.cdepth == 0)
	{
	  gc.kind = CX_GLOBAL;
	  gc.cont.glob = TRUE;
	}
    }
  else 
    {
      if (gc.kind == CX_GLOBAL)
	{
	  llcontbuglit ("Attempt to exit global context");
	  return;
	}
    }

    usymtab_exitScope (exp);
}


void
context_enterStructInnerContext (void)
{
  if (gc.kind == CX_GLOBAL)
    {
      gc.kind = CX_INNER;
      gc.cont.cdepth = 1;
    }
  else if (gc.kind == CX_INNER)
    {
      gc.cont.cdepth++;
    }
  else
    {
      ;
    }

  usymtab_enterScope ();
}

void
context_exitStructInnerContext (void)
{
  if (gc.kind == CX_INNER)
    {
      if (--gc.cont.cdepth == 0)
	{
	  gc.kind = CX_GLOBAL;
	  gc.cont.glob = TRUE;
	}
    }
  else 
    {
      if (gc.kind == CX_GLOBAL)
	{
	  llcontbuglit ("Attempt to exit global context");
	  return;
	}
    }

  usymtab_exitScope (exprNode_undefined);
}

void
context_exitInnerSafe (void)
{
  
  if (gc.kind == CX_INNER)
    {
      if (--gc.cont.cdepth == 0)
	{
	  gc.cont.cdepth++;
	}
    }
  else if (gc.kind == CX_GLOBAL)
    {
      llcontbuglit ("Attempt to exit global context");
      return;
    }
  else
    {
      if (usymtab_inDeepScope ())
	{
	  usymtab_exitScope (exprNode_undefined);
	}
    }
}

static
void setModuleAccess (void)
{
  gc.facct = typeIdSet_emptySet ();

  if (fileId_isValid (currentFile ()))
    {
      cstring baseName = fileloc_getBase (g_currentloc);
      
      if (context_getFlag (FLG_ACCESSFILE))
	{
	  if (usymtab_existsType (baseName))
	    {
	      gc.facct = typeIdSet_insert (gc.facct, 
					   usymtab_getTypeId (baseName));
	    }
	  else 
	    {
	      ;
	    }
	}
      
      if (context_getFlag (FLG_ACCESSMODULE))
	{
	  int i;
	  bool hasaccess = FALSE;
	  
	  for (i = 0; i < gc.nmods; i++)
	    {
	      if (cstring_equal (baseName, gc.moduleaccess[i].file))
		{
		  gc.facct = typeIdSet_union (gc.facct, gc.moduleaccess[i].daccess);

		  hasaccess = TRUE;
		  break;
		}
	    }
	}
      
      gc.acct = gc.facct;
      gc.inheader = fileId_isHeader (currentFile ());
    }
  else
    {
      llcontbuglit ("Current file not defined\n");
      gc.facct = typeIdSet_emptySet ();
      gc.acct = gc.facct;
      gc.inheader = FALSE;
    }
  
  /* 17 Jan 1995: forgot to clear nacct */
  
  gc.nacct = typeIdSet_emptySet ();
}

static void
context_enterFileAux (void)
{
  setModuleAccess ();
}

void
context_enterFile (void)
{
  context_enterFileAux ();
  usymtab_enterFile ();
}

void
context_enterMacroFile (void)
{
  context_enterFileAux ();
}

bool 
context_inFunction (void)
{
  kcontext ck = gc.kind;
  
  return ((ck == CX_FUNCTION) || (ck == CX_MACROFCN) || (ck == CX_INNER));
}

bool 
context_inFunctionLike (void)
{
  return (gc.kind == CX_FUNCTION || gc.kind == CX_MACROFCN 
	  || gc.kind == CX_UNKNOWNMACRO || gc.kind == CX_ITERDEF);
}

bool 
context_inRealFunction (void)
{
  kcontext ck = gc.kind;
  
  return ((ck == CX_FUNCTION) || (ck == CX_MACROFCN));
}
  
void
context_processAllMacros (void)
{
  usymtab_enterFile ();

  gc.inmacrocache = TRUE; 
  macrocache_processUndefinedElements (gc.mc);
  cleanupMessages ();  
  usymtab_exitFile ();

  gc.inmacrocache = FALSE;
  macrocache_finalize ();
}

/*
** this happens once at the end of each C file
**
** check each Macro that was defined in current file.c or current file.h
**
*/

static void
context_processMacros (void)
{
  if (fileId_isValid (currentFile ()))
    {
      fileloc lastfl;
      cstring cbase = cstring_fromChars 
	(removePathFree
	 (removeAnyExtension 
	  (cstring_toCharsSafe (fileName (currentFile ())))));
      
      gc.inmacrocache = TRUE;

      DPRINTF (("Processing macros: %s", cbase));
      lastfl = macrocache_processFileElements (gc.mc, cbase);
      DPRINTF (("Processing macros: %s", fileloc_unparse (lastfl)));

      cstring_free (cbase);
      
      if (fileloc_isDefined (lastfl))
	{
	  g_currentloc = fileloc_update (g_currentloc, lastfl);
	  cleanupMessages ();
	}

      gc.inmacrocache = FALSE;
    }
}

bool
context_processingMacros (void)
{
  return (gc.inmacrocache);
}

void
context_exitFile (void)
{
  if (gc.kind != CX_GLOBAL)
    {
      llfatalerrorLoc
	(cstring_makeLiteral ("File ended outside global scope"));
    }

  if (gc.insuppressregion)
    {
     /* gack...don't reverse the order of these lines! ;-> */
      gc.insuppressregion = FALSE;
      llerrorlit (FLG_SYNTAX, 
		  "File ended in ignore errors region, "
		  "possible missing /*@end*/");
    }

  /* fix up parse errors */

  while (!usymtab_inFileScope ())
    {
      usymtab_quietExitScope (g_currentloc);
    }

  /*
  ** Clear the file-specific modifies information.
  */
  
  sRefSetList_elements (gc.modrecs, mods)
    {
      sRefSet_clearStatics (mods);
          } end_sRefSetList_elements ;
  
  sRefSetList_clear (gc.modrecs);

  context_processMacros ();
  cleanupMessages (); 

  usymtab_exitFile ();

  gc.inDerivedFile = FALSE;
  filelocStack_clear (gc.locstack);

  gc.nacct = typeIdSet_emptySet (); /* empty noaccess */

  gc.cont.glob = TRUE;
  
  if (gc.savedFlags)
    {
      context_restoreFlagSettings ();
      gc.savedFlags = FALSE;
    }
}

void
context_exitMacroCache (void)
{
  if (gc.kind != CX_GLOBAL)
    {
      if (context_inMacro ()) /* this is okay, file could end without newline in macro */
	{
	  context_exitFunction ();
	}
      else
	{
	  llcontbug (message ("context_exitMacroCache: outside global scope: %q", 
			      context_unparse ()));
	  gc.kind = CX_GLOBAL; 
	}
    }

  /*
  ** no longer valid here
  ** if (gc.insuppressregion)
  **   {
  **     gc.insuppressregion = FALSE;
  **     llerror ("File ended in ignore errors region, possible missing @");
  **   }
  */

  gc.cont.glob = TRUE;
}

void
context_saveLocation (void)
{
  /* was llassert (fileloc_isUndefined (gc.saveloc)) */
      fileloc_free (gc.saveloc);
    

  gc.saveloc = fileloc_copy (g_currentloc);
  }

fileloc
context_getSaveLocation (void)
{
  fileloc fl = gc.saveloc;

    gc.saveloc = fileloc_undefined;
  return fl;
}

/*@observer@*/ cstring
context_inFunctionName (void)
{
  if (gc.kind == CX_FUNCTION
      || gc.kind == CX_MACROFCN || gc.kind == CX_UNKNOWNMACRO 
      || gc.kind == CX_MACROCONST 
      || gc.kind == CX_ITERDEF || gc.kind == CX_ITEREND)
    {
      return (uentry_rawName (gc.cont.fcn));
    }
  else
    {
      llcontbuglit ("context_inFunctionName: not in function");
      return (cstring_undefined);
    }
}

void
context_userSetFlag (flagcode f, bool b)
{
  DPRINTF (("set flag: %s", flagcode_name (f)));

  if (f == FLG_NEVERINCLUDE && b)
    {
      if (gc.flags[FLG_EXPORTHEADER])
	{
	  flagWarning (cstring_makeLiteralTemp
		       ("setting +neverinclude after +exportheader.  "
			"Turning off exportheader, since headers are not checked "
			"when +neverinclude is used."));

	  gc.flags[FLG_EXPORTHEADER] = FALSE;
	}
    }
  else 
    {
      if (f == FLG_EXPORTHEADER && b)
	{
	  if (gc.flags[FLG_NEVERINCLUDE])
	    {
	      flagWarning (cstring_makeLiteralTemp
			   ("setting +exportheader after +neverinclude.  "
			    "Not setting exportheader, since headers are not checked "
			    "when +neverinclude is used."));
	      gc.flags[FLG_EXPORTHEADER] = FALSE;
	      return;
	    }
	}
    }
  
  if (context_getFlag (FLG_WARNFLAGS) && f != FLG_NOF && f != FLG_OPTF)
    {
      bool lastsetting = context_getFlag (f);
      
      if (bool_equal (lastsetting, b)
	  && !flagcode_isSpecialFlag (f) 
	  && !flagcode_isIdemFlag (f)
	  && !flagcode_hasArgument (f))
	{
	  cstring warn = message ("setting %s%s redundant with current value", 
				  cstring_makeLiteralTemp (b ? "+" : "-"),
				  flagcode_name (f));
	  flagWarning (warn);
	  cstring_free (warn);
	}
    }

  if (flagcode_isLibraryFlag (f)) 
    {
      if (gc.library != FLG_ANSILIB
	  && gc.library != f)
	{
	  cstring warn = message ("selecting library %s after library %s was "
				  "selected (only one library may be used)",
				  flagcode_name (f),
				  flagcode_name (gc.library));
	  flagWarning (warn);
	  cstring_free (warn);
	}

      if (f == FLG_UNIXLIB)
	{
	  if (context_getFlag (FLG_WARNUNIXLIB))
	    {
	      flagWarning (cstring_makeLiteralTemp 
			   ("selecting unix library.  Unix library is "
			    "ad hoc addition to POSIX library.  Recommend "
			    "use +posixlib to select POSIX library instead. "
			    "Use -warnunixlib to suppress this message."));
	    }
	}
      
      gc.library = f;
    }
  
  gc.setGlobally[f] = TRUE;
  context_setFlag (f, b);
}

void
context_fileSetFlag (flagcode f, ynm set)
{
  if (!gc.savedFlags)
    {
      context_saveFlagSettings ();
    }

  if (ynm_isOff (set))
    {
      context_setFlagAux (f, FALSE, TRUE, FALSE);
    }
  else if (ynm_isOn (set))
    {
      context_setFlagAux (f, TRUE, TRUE, FALSE);
      gc.setLocally[f] = TRUE;
    }
  else
    {
      context_restoreFlag (f);
    }
}

static void
context_restoreFlag (flagcode f)
{
  
  if (!gc.savedFlags)
    {
      voptgenerror 
	(FLG_SYNTAX,
	 message ("Attempt to restore flag %s when no file scope flags "
		  "have been set.",
		  flagcode_unparse (f)),
	 g_currentloc);
    }
  else
    {
      context_addFlagMarker (f, MAYBE);
      context_setFlagAux (f, gc.saveflags[f], FALSE, TRUE);
    }

  }

static void
context_setFlag (flagcode f, bool b)
{
  context_setFlagAux (f, b, FALSE, FALSE);
}

void
context_setFlagTemp (flagcode f, bool b)
{
  DPRINTF (("Set flag temp: %s / %s", flagcode_unparse (f), bool_unparse (b)));
  gc.flags[f] = b;
}

/*@notfunction@*/
# define DOSET(ff,b) \
   do { if (inFile) { gc.setLocally[ff] = TRUE; \
		      context_addFlagMarker (ff, ynm_fromBool (b)); } \
        DPRINTF (("set flag: %s / %s", flagcode_name (ff), bool_unparse (b))); \
        gc.flags[ff] = b; } while (FALSE)

static void
  context_setFlagAux (flagcode f, bool b, bool 
		      inFile, /*@unused@*/ bool isRestore)
{
  DPRINTF (("set flag: %s / %s", flagcode_unparse (f), bool_unparse (b)));

  if (f == FLG_USESTDERR) 
    {
      if (b) {
	g_msgstream = stderr;
      } else {
	g_msgstream = stdout;
      }
    }

  if (flagcode_isSpecialFlag (f))
    {
      gc.flags[f] = b;

      
      switch (f)
	{     
	case FLG_ALLEMPTY:
	  DOSET (FLG_ALLEMPTY, b);
	  DOSET (FLG_IFEMPTY, b);
	  DOSET (FLG_WHILEEMPTY, b);
	  DOSET (FLG_FOREMPTY, b);
	  break;
	case FLG_PREDBOOL:
	  DOSET (FLG_PREDBOOL, b);
	  DOSET (FLG_PREDBOOLINT, b);
	  DOSET (FLG_PREDBOOLPTR, b);
	  DOSET (FLG_PREDBOOLOTHERS, b);
	  break;
	case FLG_GLOBALIAS:
	  DOSET (FLG_CHECKSTRICTGLOBALIAS, b);
	  DOSET (FLG_CHECKEDGLOBALIAS, b);
	  DOSET (FLG_CHECKMODGLOBALIAS, b);
	  DOSET (FLG_UNCHECKEDGLOBALIAS, b);
	  break;
	case FLG_ALLBLOCK:
	  DOSET (FLG_ALLBLOCK, b);
	  DOSET (FLG_IFBLOCK, b);
	  DOSET (FLG_WHILEBLOCK, b);
	  DOSET (FLG_FORBLOCK, b);
	  break;
	case FLG_GRAMMAR:
	  if (b)
	    {
	      yydebug = 1;
	    }
	  else
	    {
	      yydebug = 0;
	    }
	  
	  DOSET (FLG_GRAMMAR, b);
	  break;
	case FLG_CODEIMPONLY:
	  DOSET (FLG_CODEIMPONLY, b);
	  DOSET (FLG_GLOBIMPONLY, b);
	  DOSET (FLG_RETIMPONLY, b);
	  DOSET (FLG_STRUCTIMPONLY, b);
	  break;
	case FLG_SPECALLIMPONLY:
	  	  DOSET (FLG_SPECALLIMPONLY, b);
	  DOSET (FLG_SPECGLOBIMPONLY, b);
	  DOSET (FLG_SPECRETIMPONLY, b);
	  DOSET (FLG_SPECSTRUCTIMPONLY, b);
	  break;
	case FLG_ALLIMPONLY:
	  DOSET (FLG_ALLIMPONLY, b);
	  DOSET (FLG_GLOBIMPONLY, b);
	  DOSET (FLG_RETIMPONLY, b);
	  DOSET (FLG_STRUCTIMPONLY, b);
	  DOSET (FLG_SPECGLOBIMPONLY, b);
	  DOSET (FLG_SPECRETIMPONLY, b);
	  DOSET (FLG_SPECSTRUCTIMPONLY, b);
	  break;
	case FLG_ANSILIMITS: 
	  DOSET (FLG_ANSILIMITS, b);
	  DOSET (FLG_CONTROLNESTDEPTH, b);
	  DOSET (FLG_STRINGLITERALLEN, b);
	  DOSET (FLG_INCLUDENEST, b);
	  DOSET (FLG_NUMSTRUCTFIELDS, b);
	  DOSET (FLG_NUMENUMMEMBERS, b);
	  
	  if (b)
	    {
	      context_setValue (FLG_CONTROLNESTDEPTH, DEFAULT_CONTROLNESTDEPTH);
	      context_setValue (FLG_STRINGLITERALLEN, DEFAULT_STRINGLITERALLEN);
	      context_setValue (FLG_INCLUDENEST, DEFAULT_INCLUDENEST);
	      context_setValue (FLG_NUMSTRUCTFIELDS, DEFAULT_NUMSTRUCTFIELDS);
	      context_setValue (FLG_NUMENUMMEMBERS, DEFAULT_NUMENUMMEMBERS);
	    }
	  break;
	case FLG_EXTERNALNAMELEN:
	  DOSET (FLG_DISTINCTEXTERNALNAMES, TRUE);
	  DOSET (FLG_EXTERNALNAMELEN, TRUE);
	  break;
	case FLG_INTERNALNAMELEN:
	  DOSET (FLG_DISTINCTINTERNALNAMES, TRUE);
	  DOSET (FLG_INTERNALNAMELEN, TRUE);
	  break;
	case FLG_EXTERNALNAMECASEINSENSITIVE:
	  DOSET (FLG_EXTERNALNAMECASEINSENSITIVE, b);

	  if (b && !gc.flags[FLG_DISTINCTEXTERNALNAMES])
	    {
	      DOSET (FLG_DISTINCTEXTERNALNAMES, TRUE);
	      context_setValue (FLG_EXTERNALNAMELEN, 0);
	    }
	  break;
	case FLG_INTERNALNAMECASEINSENSITIVE:
	  DOSET (FLG_INTERNALNAMECASEINSENSITIVE, b);

	  if (b && !gc.flags[FLG_DISTINCTINTERNALNAMES])
	    {
	      DOSET (FLG_DISTINCTINTERNALNAMES, TRUE);
	      context_setValue (FLG_INTERNALNAMELEN, 0);
	    }
	  break;
	case FLG_INTERNALNAMELOOKALIKE:
	  DOSET (FLG_INTERNALNAMELOOKALIKE, b);

	  if (b && !gc.flags[FLG_DISTINCTINTERNALNAMES])
	    {
	      DOSET (FLG_DISTINCTINTERNALNAMES, TRUE);
	      context_setValue (FLG_INTERNALNAMELEN, 0);
	    }
	  break;
	case FLG_MODUNSPEC:
	  DOSET (FLG_MODNOMODS, b);
	  DOSET (FLG_MODGLOBSUNSPEC, b);
	  DOSET (FLG_MODSTRICTGLOBSUNSPEC, b);
	  break;
	case FLG_EXPORTANY: 
	  DOSET (FLG_EXPORTVAR, b);
	  DOSET (FLG_EXPORTFCN, b);
	  DOSET (FLG_EXPORTTYPE, b);
	  DOSET (FLG_EXPORTMACRO, b);
	  DOSET (FLG_EXPORTCONST, b);
	  gc.anyExports = TRUE;
	  break;
	case FLG_REPEXPOSE:
	  DOSET (FLG_RETEXPOSE, b); 
	  DOSET (FLG_ASSIGNEXPOSE, b); 
	  DOSET (FLG_CASTEXPOSE, b); 
	  break;
	case FLG_RETVAL:
	  DOSET (FLG_RETVALBOOL, b);
	  DOSET (FLG_RETVALINT, b);
	  DOSET (FLG_RETVALOTHER, b);
	  break;
	case FLG_PARTIAL:
	  if (b)
	    {
	      DOSET (FLG_EXPORTLOCAL, FALSE);
	      DOSET (FLG_DECLUNDEF, FALSE);
	      DOSET (FLG_SPECUNDEF, FALSE);
	      DOSET (FLG_TOPUNUSED, FALSE);
	    }
	  break;
	case FLG_DEEPBREAK:
	  DOSET (FLG_LOOPLOOPBREAK, b);
	  DOSET (FLG_LOOPSWITCHBREAK, b);
	  DOSET (FLG_SWITCHLOOPBREAK, b);
	  DOSET (FLG_SWITCHSWITCHBREAK, b);
	  DOSET (FLG_LOOPLOOPCONTINUE, b);
	  DOSET (FLG_DEEPBREAK, b);
	  break;
	case FLG_ACCESSALL:
	  DOSET (FLG_ACCESSMODULE, b);
	  DOSET (FLG_ACCESSFILE, b);
	  DOSET (FLG_ACCESSCZECH, b);
	  break;
	case FLG_ALLMACROS:
	  DOSET (FLG_ALLMACROS, b);
	  DOSET (FLG_FCNMACROS, b);
	  DOSET (FLG_CONSTMACROS, b);
	  break;
	case FLG_CZECH:
	  if (b) { DOSET (FLG_ACCESSCZECH, b); }
	  DOSET (FLG_CZECHFUNCTIONS, b);
	  DOSET (FLG_CZECHVARS, b);
	  DOSET (FLG_CZECHCONSTANTS, b);
	  DOSET (FLG_CZECHTYPES, b);
	  break;
	case FLG_SLOVAK:
	  if (b) { DOSET (FLG_ACCESSSLOVAK, b); }
	  DOSET (FLG_SLOVAKFUNCTIONS, b);
	  DOSET (FLG_SLOVAKVARS, b);
	  DOSET (FLG_SLOVAKCONSTANTS, b);
	  DOSET (FLG_SLOVAKTYPES, b);
	  break;
	case FLG_CZECHOSLOVAK:
	  if (b) { DOSET (FLG_ACCESSCZECHOSLOVAK, b); }
	  DOSET (FLG_CZECHOSLOVAKFUNCTIONS, b);
	  DOSET (FLG_CZECHOSLOVAKVARS, b);
	  DOSET (FLG_CZECHOSLOVAKCONSTANTS, b);
	  DOSET (FLG_CZECHOSLOVAKTYPES, b);
	  break;
	case FLG_NULL:
	  DOSET (FLG_NULLSTATE, b);
	  DOSET (FLG_NULLDEREF, b);
	  DOSET (FLG_NULLASSIGN, b);
	  DOSET (FLG_NULLPASS, b);
	  DOSET (FLG_NULLRET, b);
	  break;
	case FLG_MEMCHECKS:
	  DOSET (FLG_NULLSTATE, b);
	  DOSET (FLG_NULLDEREF, b);
	  DOSET (FLG_NULLASSIGN, b);
	  DOSET (FLG_NULLPASS, b);
	  DOSET (FLG_NULLRET, b);
	  DOSET (FLG_COMPDEF, b);
	  DOSET (FLG_COMPMEMPASS, b);
	  DOSET (FLG_UNIONDEF, b);
	  DOSET (FLG_MEMTRANS, b);
	  DOSET (FLG_USERELEASED, b);
	  DOSET (FLG_ALIASUNIQUE, b);
	  DOSET (FLG_MAYALIASUNIQUE, b);
	  DOSET (FLG_MUSTFREE, b);
	  DOSET (FLG_MUSTDEFINE, b);
	  DOSET (FLG_GLOBSTATE, b); 
	  DOSET (FLG_COMPDESTROY, b);
	  DOSET (FLG_MUSTNOTALIAS, b);
	  DOSET (FLG_MEMIMPLICIT, b);
	  DOSET (FLG_BRANCHSTATE, b); 
	  /*@fallthrough@*/ /* also sets memtrans flags */
	case FLG_MEMTRANS:
	  DOSET (FLG_MEMTRANS, b);
	  DOSET (FLG_EXPOSETRANS, b);
	  DOSET (FLG_OBSERVERTRANS, b);
	  DOSET (FLG_DEPENDENTTRANS, b);
	  DOSET (FLG_NEWREFTRANS, b);
	  DOSET (FLG_ONLYTRANS, b);
	  DOSET (FLG_OWNEDTRANS, b);
	  DOSET (FLG_FRESHTRANS, b);
	  DOSET (FLG_SHAREDTRANS, b);
	  DOSET (FLG_TEMPTRANS, b);
	  DOSET (FLG_KEPTTRANS, b);
	  DOSET (FLG_REFCOUNTTRANS, b);
	  DOSET (FLG_STATICTRANS, b);
	  DOSET (FLG_UNKNOWNTRANS, b);
	  DOSET (FLG_KEEPTRANS, b);
	  DOSET (FLG_IMMEDIATETRANS, b);
	  break;

	default:
	  llcontbug (message ("Unhandled special flag: %s", 
			      flagcode_unparse (f)));
	  break;
	}
    }
  else
    {
      if (flagcode_isIdemFlag (f))
	{
	  DOSET (f, TRUE);
	}
      else
	{
	  if (b && !gc.anyExports
	      && (f == FLG_EXPORTVAR || f == FLG_EXPORTFCN
		  || f == FLG_EXPORTTYPE || f == FLG_EXPORTMACRO
		  || f == FLG_EXPORTCONST
		  || f == FLG_EXPORTANY))
	    {
	      gc.anyExports = TRUE;
	    }

	  DOSET (f, b);
	}
    }
}

bool 
context_maybeSet (flagcode d)
{
  return (gc.flags[d] || gc.setLocally[d]);
}

bool
context_getFlag (flagcode d)
{
  return (gc.flags[d]);
}

static void context_saveFlagSettings (void)
{
  gc.savedFlags = TRUE;
  llassert (sizeof (gc.saveflags) == sizeof (gc.flags));
  memcpy (gc.saveflags, gc.flags, sizeof (gc.flags));
}

static void context_restoreFlagSettings (void)
{
  llassert (sizeof (gc.saveflags) == sizeof (gc.flags));
  memcpy (gc.flags, gc.saveflags, sizeof (gc.flags));
  gc.savedFlags = FALSE;
}

void context_setFilename (fileId fid, int lineno) 
   /*@globals fileloc g_currentloc;@*/
   /*@modifies g_currentloc@*/
{
  if (fileId_baseEqual (currentFile (), fid))
    {
      setLine (lineno);
      return;
    }
  else
    {
      fileloc_setColumn (g_currentloc, 0);

      if (fileloc_isSpecialFile (g_currentloc))
	{
	  gc.inDerivedFile = TRUE;
	}

      if (filelocStack_popPushFile (gc.locstack, g_currentloc))
	{
	  int maxdepth = context_getValue (FLG_INCLUDENEST);

	  if (filelocStack_size (gc.locstack) > maxdepth)
	    {
	      int depth = filelocStack_includeDepth (gc.locstack);
	      
	      if (depth > maxdepth)
		{
		  if (optgenerror 
		      (FLG_INCLUDENEST,
		       message ("Maximum include nesting depth "
				"(%d, current depth %d) exceeded",
				maxdepth,
				depth),
		       filelocStack_nextTop (gc.locstack)))
		    {
		      filelocStack_printIncludes (gc.locstack);
		    }
		}
	    }
	}
      
      g_currentloc = fileloc_create (fid, lineno, 1);
      gc.inheader = fileId_isHeader (currentFile ());

      context_enterFileAux ();
    }
}

void context_enterIterDef (/*@observer@*/ uentry le)
{
    context_enterMacro (le);
  gc.acct = typeIdSet_subtract (gc.facct, gc.nacct);
  gc.kind = CX_ITERDEF;
}

void context_enterIterEnd (/*@observer@*/ uentry le)
{
    context_enterMacro (le);
  gc.kind = CX_ITEREND;
}

void 
context_destroyMod (void) 
   /*@globals killed gc@*/
{
  setCodePoint ();
  ctype_destroyMod ();
  setCodePoint ();
  usymtab_free ();
  setCodePoint ();
  fileTable_free (gc.ftab);
  filelocStack_free (gc.locstack);
  setCodePoint ();
  gc.ftab = fileTable_undefined;

  macrocache_free (gc.mc);
  sfree (gc.moduleaccess);
  setCodePoint ();

  fileloc_free (gc.saveloc); gc.saveloc = fileloc_undefined;
  fileloc_free (gc.pushloc); gc.pushloc = fileloc_undefined;

  setCodePoint ();
  sRefSetList_free (gc.modrecs);
  setCodePoint ();
  flagMarkerList_free (gc.markers); 
  setCodePoint ();
  messageLog_free (gc.msgLog);
  setCodePoint ();
  clauseStack_free (gc.clauses);
  setCodePoint ();
  
  cstring_free (gc.msgAnnote);
  globSet_free (gc.globs_used);
  }

/*
** Flag shortcuts.
*/

bool context_msgBoolInt (void)
{
  return gc.flags [FLG_BOOLINT];
}

bool context_msgCharInt (void)
{
  return gc.flags [FLG_CHARINT];
}

bool context_msgEnumInt (void)
{
  return gc.flags [FLG_ENUMINT];
}

bool context_msgPointerArith (void) 
{
  return gc.flags [FLG_POINTERARITH];
}

bool context_msgStrictOps (void) 
{
  return gc.flags [FLG_STRICTOPS];
}

# ifndef NOLCL
bool context_msgLh (void)           
{
  return gc.flags [FLG_DOLH];
}
# endif

void context_pushLoc (void) 
{
  fileloc_free (gc.pushloc);
  gc.pushloc = gc.saveloc;
  gc.saveloc = fileloc_undefined;
}

void context_popLoc (void) 
{
  gc.saveloc = fileloc_update (gc.saveloc, gc.pushloc);
}

bool context_inGlobalScope (void)
{
  return (usymtab_inFileScope() || usymtab_inGlobalScope ());
}

bool context_inInnerScope (void)
{
  return (gc.kind == CX_INNER);
}

void context_setProtectVars (void)
{
  gc.protectVars = TRUE;
}

bool context_anyErrors (void)
{
  return (gc.numerrors > 0);
}

void context_hasError (void)
{
  gc.numerrors++;
}

int context_numErrors (void)
{
  return gc.numerrors;
}

bool context_neednl (void)
{
  return gc.neednl;
}

void context_setNeednl (void)
{
  gc.neednl = TRUE;
}

int context_getExpect (void)
{
  return (context_getValue (FLG_EXPECT));
}

# ifndef NOLCL
int context_getLCLExpect (void)
{
  return (context_getValue (FLG_LCLEXPECT));
}
# endif

int context_getLimit (void)
{
  return (context_getValue (FLG_LIMIT));
}

bool context_unlimitedMessages (void)
{
  return (context_getLimit () < 0);
}

void context_releaseVars (void)
{
  llassert (gc.protectVars);
  gc.protectVars = FALSE;
}

void context_sizeofReleaseVars (void)
{
  /* If there is a nested sizeof, this might not hold:
     llassert (gc.protectVars);
     */

  gc.protectVars = FALSE;
}

bool context_inProtectVars (void)
{
  return (gc.protectVars);
}

void context_hideShowscan (void) 
{
  gc.flags[FLG_SHOWSCAN] = FALSE;
}

void context_unhideShowscan (void)
{
  gc.flags[FLG_SHOWSCAN] = TRUE;
}

bool context_inHeader (void)
{
  return (gc.inheader);
}

fileTable context_fileTable (void)
{
  return gc.ftab;
}

cstring context_tmpdir (void)
{
  return (context_getString (FLG_TMPDIR));
}

messageLog context_messageLog (void)
{
  return gc.msgLog;
}

bool context_inMacroFunction (void)
{
  return (gc.kind == CX_MACROFCN);
}

bool context_inMacroConstant (void)
{   
  return (gc.kind == CX_MACROCONST);
}

bool context_inMacroUnknown (void)
{   
  return (gc.kind == CX_UNKNOWNMACRO);
}

void context_setShownFunction (void)
{
  gc.showfunction = FALSE;
}

bool context_doDump (void)
{   
  return cstring_isNonEmpty (context_getString (FLG_DUMP));
}

bool context_doMerge (void)
{
  return cstring_isNonEmpty (context_getString (FLG_MERGE));
}

cstring context_getDump (void)
{           
  return context_getString (FLG_DUMP);
}

cstring context_getMerge (void)
{
  return context_getString (FLG_MERGE);
}

# ifndef NOLCL
bool context_inLCLLib (void)
{   
  return (gc.kind == CX_LCLLIB);
}

bool context_inImport (void)
{
  return (gc.inimport);
}

void context_enterImport (void)
{ 
  gc.inimport = TRUE;
}

void context_leaveImport (void)
{ 
  gc.inimport = FALSE;
}
# endif

bool context_inMacro (void) 
{
  return (gc.kind == CX_MACROFCN || gc.kind == CX_MACROCONST 
	  || gc.kind == CX_UNKNOWNMACRO
	  || gc.kind == CX_ITERDEF || gc.kind == CX_ITEREND);
}

bool context_inIterDef (void)
{
  return (gc.kind == CX_ITERDEF);
}

bool context_inIterEnd (void)
{
  return (gc.kind == CX_ITEREND);
}

int context_getLinesProcessed (void)    
{
  return (gc.linesprocessed);
}

int context_getSpecLinesProcessed (void)    
{
  return (gc.speclinesprocessed);
}

# ifndef NOLCL
void context_processedSpecLine (void)
{
  gc.speclinesprocessed++;
}

void context_resetSpecLines (void)    
{
  gc.speclinesprocessed = 0;
}
# endif

bool context_inGlobalContext (void)
{
  return (gc.kind == CX_GLOBAL);
}

void context_setFileId (fileId s)
{
  g_currentloc = fileloc_updateFileId (g_currentloc, s); 
}

bool context_setBoolName (void)
{
  return (!cstring_equalLit (context_getString (FLG_BOOLTYPE),
			     DEFAULT_BOOLTYPE));
}

cstring context_printBoolName (void)
{
  if (context_setBoolName ()) 
    {
      return context_getBoolName ();
    }
  else
    {
      return cstring_makeLiteralTemp ("boolean");
    }
}

cstring context_getBoolName (void)
{
  return (context_getString (FLG_BOOLTYPE));
}

cstring context_getFalseName (void)
{
  return (context_getString (FLG_BOOLFALSE));
}

cstring context_getTrueName (void)
{
  return (context_getString (FLG_BOOLTRUE));
}

cstring context_getLarchPath (void)
{
  return (context_getString (FLG_LARCHPATH));
}

cstring context_getLCLImportDir (void)
{
  return (context_getString (FLG_LCLIMPORTDIR));
}

static void context_setJustPopped (void)
{
  gc.justpopped = TRUE;
}

void context_clearJustPopped (void)
{
  gc.justpopped = FALSE;
}

bool context_justPopped (void)
{
  return (gc.justpopped);
}

void context_setMacroMissingParams (void)
{
  gc.macroMissingParams = TRUE;
}

void context_resetMacroMissingParams (void)
{
  gc.macroMissingParams = FALSE;
}

bool context_isMacroMissingParams (void)
{
  return (gc.macroMissingParams);
}

void context_showFilelocStack (void) 
{
  filelocStack_printIncludes (gc.locstack);
}






