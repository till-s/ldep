/* $Id$ */

/* tool for library/object file dependency analysis */

/* Author: Till Straumann <strauman@slac.stanford.edu>, 2003 */

/* scan symbol tables generated by 'nm -f posix'
 * obeying the format:
 *
 * <library_name>'['<archive_member_name>']:'
 * <symbol_name>' '<class_char>' '[<start>' '<end>]
 *
 * The tool builds a database of all object files and another
 * one containing all symbols.
 *
 * Each object file holds lists of pointers to all symbols
 * it imports and exports, respectively.
 *
 * Each symbol object holds a pointer to the object where
 * it is defined and a list of objects importing the symbol.
 *
 * Using these datastructures, the tool can 'link' objects
 * together and construct dependency information.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <search.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

/* some debugging flags are actually 'verbosity' flags
 * and are also used if DEBUG is undefined
 */
#define DEBUG_SCAN		(1<<0)
#define DEBUG_TREE		(1<<1)
#define DEBUG_WALK		(1<<2)
#define DEBUG_LINK		(1<<3)
#define DEBUG_UNLINK	(1<<4)

#define DEBUG			(DEBUG_LINK | DEBUG_UNLINK)
#define NWORK

static int	verbose =
#ifdef DEBUG
	DEBUG
#else
	0
#endif
	;

static int	force = 0;

typedef struct ObjFRec_		*ObjF;
typedef struct SymRec_		*Sym;
typedef struct XrefRec_		*Xref;
typedef struct LinkSetRec_	*LinkSet;
typedef struct LibRec_		*Lib;


typedef struct LinkSetRec_ {
	char	*name;
	ObjF	set;
} LinkSetRec;

typedef struct LinkNodeRec {
	LinkSet	anchor;
	ObjF	next;
} LinkNodeRec;

typedef struct ObjFRec_ {
	char		*name;		/* name of this object file */
	ObjF		next;		/* linked list of all objects */
	Lib			lib;		/* libary we're part of or NULL */
	LinkNodeRec link; 		/* link set */
	ObjF		work;		/* temp pointer to do work */
#ifndef NWORK
	ObjF		work1;
#endif
	int			nexports;
	Xref		exports;	/* symbols exported by this object */
	int			nimports;
	Xref		imports;	/* symbols exported by this object */
} ObjFRec;

typedef struct LibRec_ {
	char	*name;
	Lib		next;		/* linked list of libraries */
	int		nfiles;
	ObjF	*files;		/* pointer array of library members */
} LibRec;

#define TYPE(sym) (*((sym)->name - 1))

#ifdef  TYPE
#define TYPESZ	1
#else
#define TYPESZ  0
#endif

typedef struct SymRec_ {
	char	*name;		/* we point 'name' to a string and store the type in 'name[-1]' */
	Xref	exportedBy;
	Xref	importedFrom;
} SymRec;

typedef struct XrefRec_ {
	Sym			sym;
	ObjF		obj;
	unsigned	multiuse;	/* BITFIELD (assuming pointers are word aligned; LSB is 'weak' flag) */
} XrefRec;

#define XREF_FLAGS 		1
#define XREF_FLG_WEAK	1

#define XREF_NEXT(ref) ((Xref)((ref)->multiuse & ~XREF_FLAGS))
#define XREF_WEAK(ref) ((ref)->multiuse & XREF_FLG_WEAK)

#define WARN_UNDEFINED_SYMS (1<<0)

#ifndef DEFAULT_WARN_FLAGS
#ifdef DEBUG
#define DEFAULT_WARN_FLAGS (-1)
#else
#define DEFAULT_WARN_FLAGS 0
#endif
#endif

static int warn = DEFAULT_WARN_FLAGS;

static __inline__ void xref_set_next(Xref e, Xref next)
{
	e->multiuse &= XREF_FLAGS;
	e->multiuse |= ((unsigned)next) & ~XREF_FLAGS;
}

static __inline__ void xref_set_weak(Xref e, int weak)
{
	if (weak)
		e->multiuse |= XREF_FLG_WEAK;
	else
		e->multiuse &= ~XREF_FLG_WEAK;
}

typedef void (*DepWalkAction)(ObjF f, int depth, void *closure);

/* mode bits */
#define WALK_BUILD_LIST	(1<<0)
#define WALK_EXPORTS	(1<<1)
#define WALK_IMPORTS	(0)


typedef struct DepPrintArgRec_ {
	int		minDepth;
	int 	indent;
	int 	depthIndent;
	FILE	*file;
} DepPrintArgRec, *DepPrintArg;

/* forward declarations */
static void depwalk_rec(ObjF f, int depth);
static void depPrint(ObjF f, int depth, void *closure);
void depwalk(ObjF f, DepWalkAction action, void *closure, int mode);
int  checkCircWorkList(ObjF f);
void depwalkListRelease(ObjF f);
void workListIterate(ObjF f, DepWalkAction action, void *closure);

extern LinkSetRec appLinkSet;
extern LinkSetRec undefLinkSet;
extern LinkSetRec optionalLinkSet;

static FILE *debugf, *logf;

#define STRCHUNK	10000

/* string space allocator */
char *stralloc(int len)
{
static char		*buf;
static int		avail=0;
char			*rval;

	assert(len<=STRCHUNK);

	if (len > avail) {
		avail = STRCHUNK;
		assert( buf=malloc(avail) );
	}

	rval   = buf;
	buf   += len;
	avail -= len;
	return rval;
}

#define MAXBUF	500

#define NMFMT(max)  "%"#max"s"
#define XNMFMT(m)	NMFMT(m)
#define ENDFMT		"%*[^\n]\n"
//#define FMT(max)	"%"#max"s%*[ \t]%c%*[^\n] \n"

#define THEFMT 		XNMFMT(MAXBUF)"%*[ \t]%c"ENDFMT
#define THENMFMT	XNMFMT(MAXBUF)ENDFMT

/* a "special" object exporting all symbols not defined
 * anywhere else
 */
static ObjFRec undefSymPod = {
	"<UNDEFINED>",
link: { anchor: &undefLinkSet },
};

LinkSetRec appLinkSet   = {
	name:	"Application",
	set:	0,
};
LinkSetRec undefLinkSet = {
	name:	"UNDEFINED",
	set:	&undefSymPod,
};
LinkSetRec optionalLinkSet = {
	name:	"Optional",
	set:	0
};

ObjF fileListHead=&undefSymPod, fileListTail=&undefSymPod;
Lib  libListHead=0, libListTail=0;
ObjF *fileListIndex = 0;

static __inline__ ObjF
fileListFirst()
{
	/* skip the undefined pod */
	return fileListHead ? fileListHead->next : 0;
}

static int	numFiles = 1; /* undefFilePod */
static int  numLibs  = 0;

void *symTbl = 0;

static int
symcmp(const void *a, const void *b)
{
const Sym sa=(const Sym)a, sb=(const Sym)b;
	return strcmp(sa->name, sb->name);
}

static void
fixupObj(ObjF f)
{
Sym		sym;
Xref	ex;
int		i;

	if ( !f )
		return;

	/* fixup export list; we can do this only after all reallocs have been performed */

	for (i=0, ex=f->exports; i<f->nexports; i++,ex++) {
		/* append to list of modules exporting this symbol */
		sym = ex->sym;
		if ( sym->exportedBy ) {
			Xref etmp;
			for ( etmp = sym->exportedBy; XREF_NEXT(etmp); etmp=XREF_NEXT(etmp) )
			  	/* nothing else to do */;
				xref_set_next(etmp, ex);
		} else {
			sym->exportedBy = ex;
		}
	}
}

static Lib
createLib(char *name)
{
Lib	rval;

	assert( rval = calloc(1, sizeof(*rval)) );
	assert( rval->name = stralloc(strlen(name) + 1) );
	strcpy( rval->name, name );
	if (libListTail)
		libListTail->next = rval;
	else
		libListHead	= rval;
	libListTail = rval;
	numLibs++;
	return rval;
}

void
libAddObj(char *libname, ObjF obj)
{
Lib l;
int i;
	for (l=libListHead; l; l=l->next) {
		if ( !strcmp(l->name, libname) )
			break;
	}
	if ( !l ) {
		/* must create a new library */
		l = createLib(libname);
	}
	/* sanity check */
	for ( i=0; i<l->nfiles; i++) {
		assert( strcmp(l->files[i]->name, obj->name) );
	}
	i = l->nfiles+1;
	assert( l->files = realloc(l->files, i * sizeof(*l->files)) );
	l->files[l->nfiles] = obj;
	l->nfiles = i;
	obj->lib  = l;
}

static char *
splitName(char *name, char **ppo, char **ppc)
{
char	*rval = name;
int		l     = strlen(name);

	*ppo = 0;

	/* is it part of a library ? */
	if ( l>0 && ']'== *(*ppc=name+(l-1)) ) {
		if ( !(*ppo = strrchr(name,'[')) ) {
			fprintf(stderr,"ERROR: misformed archive member name: %s\n",name);
			fprintf(stderr,"       'library[member]' expected\n");
			return 0;
		}
		**ppo  = 0;
		**ppc  = 0;
		rval =  (*ppo)+1;
	}
	return rval;
}

static int
printObjName(FILE *feil, ObjF f)
{
Lib l = f->lib;
char *lname = "";

	if ( l ) {
		if ((lname = strrchr(l->name,'/'))) {
			lname++;
		} else  {
			lname = l->name;
		}
	}
	return fprintf(feil, l ? "%s[%s]" : "%s%s", lname, f->name);
}

static ObjF
createObj(char *name)
{
ObjF obj;
int  l = strlen(name);
char *po,*pc,*objn;

	/* is it part of a library ? */
	objn = splitName(name, &po, &pc);

	if (!objn)
		exit(1); /* found an ill-formed name; fatal */

	assert( obj = calloc(1, sizeof(*obj)) );

	/* build/copy name */
	assert( obj->name = stralloc(strlen(objn) + 1) );
	strcpy( obj->name, objn );

	/* append to list of objects */
	fileListTail->next = obj;
	fileListTail = obj;
	numFiles++;

	if (po) {
		/* part of a library */
		libAddObj(name, obj);
		*po = '[';
		*pc = ']';
	}

	return obj;
}


#define TOUPPER(ch)	(force ? toupper(ch) : (ch)) /* less paranoia when checking symbol types */


int
scan_file(FILE *f, char *name)
{
char	buf[MAXBUF+1];
int		got;
char	type;
int		line=0;
ObjF	obj =0;
int		len;
int		weak;
Sym		nsym = 0,sym;
Sym		*found;

	/* tag end of buffer */
	buf[MAXBUF]='X';

	while ( EOF != (got = fscanf(f, THEFMT, buf, &type)) ) {
		line++;
		if ( !buf[MAXBUF] ) {
			fprintf(stderr,"Scanner buffer overrun\n");
			return -1;
		}
		switch (got) {
			default:
				fprintf(stderr,"Unable to read %s/line %i (%i conversions of '%s')\n",name,line,got,THEFMT);
				return -1;

			case 1:
				len = strlen(buf);
				if ( ':' != buf[len-1] ) {
					fprintf(stderr,"<FILENAME> in %s/line %i not ':' terminated - did you use 'nm -fposix?'\n", name, line);
					return -1;
				}
				fixupObj(obj);

				/* strip trailing ':' */
				buf[--len]=0;

				obj = createObj(buf);

#if DEBUG & DEBUG_SCAN
				fprintf(debugf,"In FILE: '%s'\n", buf);
#endif
			break;

			case 2:
				if (!obj) {
					char *dot, *slash,*nmbuf;
					fprintf(stderr,"Warning: Symbol without object file??\n");
				    fprintf(stderr,"-> substituting symbol file name... (%s/line %i)\n",name,line);

					assert( nmbuf = malloc(strlen(name)+5) );

					strcpy( nmbuf, name );
					slash = strchr(nmbuf, '/');
					dot   = strrchr(nmbuf, '.');
					if ( !dot || (slash && slash > dot) ) {
						strcat(nmbuf, ".o");
					} else {
						strcpy(dot+1,"o");
					}
					obj = createObj(nmbuf);
					free(nmbuf);
				}

				type = TOUPPER(type);

				if ( !nsym )
					assert( nsym = calloc(1,sizeof(*nsym)) );

				nsym->name = buf;

				assert( found = (Sym*) tsearch(nsym, &symTbl, symcmp) );
				if ( *found == nsym ) {
#if DEBUG & DEBUG_TREE
					fprintf(debugf,"Adding new symbol %s (found %p, sym %p)\n",(*found)->name, found, *found);
#endif
					nsym->name = stralloc(strlen(buf) + 1 + TYPESZ) + TYPESZ; /* store the type in name[-1] */
					strcpy(nsym->name, buf);
#ifdef TYPE
					TYPE(nsym) = (char)type;
#endif
					nsym = 0;
				} else {
#if DEBUG & DEBUG_TREE
					fprintf(debugf,"Found existing symbol %s (found %p, sym %p)\n",(*found)->name, found, *found);

#endif

#ifdef TYPE
					if (  type != TYPE(*found) ) {
						int warn, override, nweak;

						nweak= 'W' == type || 'V' == type;
#warning TODO weak symbols

						warn = ( 'U' != TYPE(*found) && 'U' != type );
						
				 		if (warn) {
							fprintf(stderr,"Warning: type mismatch between multiply defined symbols\n");
					    	fprintf(stderr,"         %s: known as %c, is now %c\n", (*found)->name, TYPE(*found), type);
						}

						override = ('U' == TYPE(*found));

						if (override) {
							TYPE(*found) = type;
						}
					}
#endif
				}
				sym = *found;

				weak = 0;

				switch ( TOUPPER(type) ) {
bail:
					default:
						fprintf(stderr,"Unknown symbol type '%c' (line %i)\n",type,line);
					return -1;

					case 'W':
					case 'V': weak = 1;
					case 'D':
					case 'T':
					case 'B':
					case 'R':
					case 'G':
					case 'S':
					case 'A':
					case 'C':
							  {
							  Xref ex;
							  obj->nexports++;
							  assert( obj->exports = realloc(obj->exports, sizeof(*obj->exports) * obj->nexports) );
							  /* check alignment with flags */
							  assert( 0 == ((unsigned)obj->exports & XREF_FLAGS) );
							  ex = &obj->exports[obj->nexports - 1];
							  ex->sym = sym;
							  ex->obj = obj;
							  xref_set_weak(ex,weak);
							  xref_set_next(ex,0);
							  }
					break;

					case '?':
							  if ( !force ) goto bail;
							  /* else: fall thru / less paranoia */

					case 'U':
							  {
							  Xref im;
							  obj->nimports++;
							  assert( obj->imports = realloc(obj->imports, sizeof(*obj->imports) * obj->nimports) );
							  /* check alignment with flags */
							  assert( 0 == ((unsigned)obj->imports & XREF_FLAGS) );
							  im = &obj->imports[obj->nimports - 1];
							  im->sym = sym;
							  im->obj = obj;
							  xref_set_weak(im,0);
							  xref_set_next(im,0);
							  }
					break;
				}
#if DEBUG & DEBUG_SCAN
				fprintf(debugf,"\t '%c' %s\n",type,buf);
#endif
			break;
		}
	}
	fixupObj(obj);
	free(nsym);
	return 0;
}

static void
gatherDanglingUndefsAct(const void *pnode, const VISIT when, const int depth)
{
const Sym sym = *(const Sym*)pnode;
	if ( (postorder == when || leaf == when) && ! sym->exportedBy) {
		Xref ex;
		undefSymPod.nexports++;
		undefSymPod.exports = realloc(undefSymPod.exports, sizeof(*undefSymPod.exports) * undefSymPod.nexports);
		/* check alignment with flags */
		assert( 0 == ((unsigned)undefSymPod.exports & XREF_FLAGS) );
		ex = &undefSymPod.exports[undefSymPod.nexports-1];
		ex->sym  = sym;
		ex->obj  = &undefSymPod;
		xref_set_weak(ex,0);
		xref_set_next(ex,0);
	}
}


/* gather symbols which are defined nowhere and attach them
 * to the export list of the 'special' object at the list head.
 */
void
gatherDanglingUndefs()
{
	twalk(symTbl, gatherDanglingUndefsAct);
	fixupObj(&undefSymPod);
}

/* semantics: caller must have asserted that
 * 'f' is not part of any linkset already
 * (f->link.anchor == 0).
 * Then, the caller sets f->link.anchor and
 * calls 'link' to perform a recursive link.
 *
 */

int
linkObj(ObjF f, char *symname)
{
register int i;
register Xref imp;

	assert(f->link.anchor);

	if (verbose & DEBUG_LINK) {
		fprintf(debugf,"Linking '"); printObjName(debugf,f); fputc('\'', debugf);
		if (symname)
			fprintf(debugf,"because of '%s'",symname);
		fprintf(debugf," to %s link set\n", f->link.anchor->name);
	}

	for (i=0, imp=f->imports; i<f->nimports; i++, imp++) {
		register Sym *found;
		assert( 0 == XREF_NEXT(imp) );
		assert (found = (Sym*)tfind( imp->sym, &symTbl, symcmp ));

		/* add ourself to the importers of that symbol */
		xref_set_next(imp, (*found)->importedFrom);
		(*found)->importedFrom = imp;

		if ( !(*found)->exportedBy ) {
			if (warn & WARN_UNDEFINED_SYMS) {
				fprintf(stderr,
					"Warning: symbol %s:%s undefined\n",
					f->name, imp->sym->name);
			}
		} else {
			ObjF	dep= (*found)->exportedBy->obj;
			if ( f->link.anchor && !dep->link.anchor ) {
				dep->link.anchor = f->link.anchor;
				linkObj(dep,(*found)->name);
			}
		}
	}

	f->link.next = (f->link.anchor->set);
	f->link.anchor->set = f;
}

void
trackSym(FILE *feil, Sym s)
{
Xref 			ex;
Xref			imp;
DepPrintArgRec	arg;

	arg.depthIndent = -2;
	arg.file		= feil;

	fprintf(feil,"What I know about Symbol '%s':\n", s->name);
	fprintf(feil,"  Defined in object: ");
	if ( ! (ex = s->exportedBy) ) {
		fprintf(feil," NOWHERE!!!\n");
	} else {
		printObjName(feil, ex->obj);
		fprintf(feil,"%s\n", XREF_WEAK(ex) ? " (WEAK)" : "");
		while ( ex=XREF_NEXT(ex) ) {
			fprintf(feil,"      AND in object: ");
			printObjName(feil, ex->obj);
			fprintf(feil,"%s\n", XREF_WEAK(ex) ? " (WEAK)" : "");
		}
	}

	if ( (ex=s->exportedBy) ) {
		fprintf(feil,"  Depending on objects (triggers linkage of):");
		if (0 == ex->obj->nimports) {
			fprintf(feil," NONE\n");
		} else {
			fprintf(feil,"\n");
			arg.minDepth    = 1;
			arg.indent      = 0;
			depwalk(ex->obj, depPrint, (void*)&arg, WALK_IMPORTS | WALK_BUILD_LIST);
			depwalkListRelease(ex->obj);
		}
	}

	fprintf(feil,"  Objects depending (maybe indirectly) on this symbol:\n");
    fprintf(feil,"  Note: the host object may depend on yet more objects due to other symbols...\n");

	imp = s->importedFrom;

	if ( imp ) {
		fprintf(feil,"\n");
		arg.minDepth    = 0;
		arg.indent      = 4;
		do {
			depwalk(imp->obj, depPrint, (void*)&arg, WALK_EXPORTS | WALK_BUILD_LIST);
			depwalkListRelease(imp->obj);
		} while ( imp = XREF_NEXT(imp) );
	} else {
		fprintf(feil," NONE\n");
	}
}

int
trackObj(FILE *feil, ObjF f)
{
int				i;
DepPrintArgRec	arg;

	fprintf(feil,"What I know about object '");
	printObjName(feil,f);
	fprintf(feil,"':\n");

	fprintf(feil,"  Exported symbols:\n");

	for ( i=0; i<f->nexports; i++)
		fprintf(feil,"    %s\n",f->exports[i].sym->name);

	fprintf(feil,"  Imported symbols:\n");

	for ( i=0; i<f->nimports; i++)
		fprintf(feil,"    %s\n",f->imports[i].sym->name);

	fprintf(feil,"  Objects depending on me (including indirect dependencies):\n");

	arg.minDepth    = 0;
	arg.indent      = 4;
	arg.depthIndent = -1;
	arg.file		= feil;

	depwalk(f, depPrint, (void*)&arg, WALK_EXPORTS | WALK_BUILD_LIST);
	depwalkListRelease(f);

	fprintf(feil,"  Objects I depend on (including indirect dependencies):\n");

	depwalk(f, depPrint, (void*)&arg, WALK_IMPORTS | WALK_BUILD_LIST);
	depwalkListRelease(f);
	
	return 0;
}

static void
doUnlink(ObjF f, int depth, void *closure)
{
Xref	imp,p,n;
int		i;
ObjF	*pl;

	if ( verbose & DEBUG_UNLINK ) {
		fprintf(debugf,"\n  removing object '");
		printObjName(debugf,f);
		fprintf(debugf,"'... ");
	}

	for (i=0, imp=f->imports; i<f->nimports; i++, imp++) {
		/* remove ourself from the list of importers of that symbol */

		/* retrieve our predecessor */
		p = imp->sym->importedFrom;
		
		if ( p == imp ) {
			imp->sym->importedFrom = XREF_NEXT(imp);
		} else {
			while ( p && ((n=XREF_NEXT(p)) != imp) ) {
				p = n;
			}
			assert( p );
			xref_set_next(p, XREF_NEXT(imp));
		}
		
		xref_set_next(imp, 0);
	}

	/* remove this object from its linkset */
	for (pl = &f->link.anchor->set; *pl && (*pl != f); pl = &((*pl)->link.next) )
		/* do nothing else */;
	assert( *pl );
	*pl = f->link.next;
	f->link.next   = 0;
	f->link.anchor = 0;

	if ( verbose & DEBUG_UNLINK )
		fprintf(debugf,"OK\n");
}

static void
checkSysLinkSet(ObjF f, int depth, void *closure)
{
	if ( f->link.anchor == &appLinkSet ) {
		if ( verbose & DEBUG_UNLINK &&  ! *(int*)closure ) {
			fprintf(debugf,"  --> rejected because '");
			printObjName(debugf,f);
			fprintf(debugf,"' is needed by app");
		}
		*(int*)closure = 1;
	}
}

static void
checkSanity(ObjF f, int depth, void *closure)
{
Xref	ex;
int		i;
	/* sanity check. All exported symbols' import lists must be empty */
	for (i=0, ex=f->exports; i<f->nexports; i++,ex++) {
		assert( ex->sym->importedFrom == 0 );
	}
}


int
unlinkObj(ObjF f)
{
int		reject = 0;

	depwalk(f, 0, 0, WALK_EXPORTS | WALK_BUILD_LIST);

	/* check if any of the objects is part of the
	 * fundamental link set
	 */
	workListIterate(f, checkSysLinkSet, &reject);

	if ( ! reject ) {
		workListIterate(f, doUnlink, 0);
		workListIterate(f, checkSanity, 0);
	}
	depwalkListRelease(f);
	return reject;
}

/* unlink all modules depending on undefined symbols;
 * (this will fail for 'system' / critical objects.
 * The reason is that some symbols still might be
 * defined by the linker script.)
 */
int
unlinkUndefs()
{
int		i;
Xref	ex,p,n;
ObjF	q = &undefSymPod;

	for (i=0, ex=q->exports; i<q->nexports; i++,ex++) {
		if ( verbose & DEBUG_UNLINK )
			fprintf(debugf,"removing objects depending on '%s'...", ex->sym->name);
		while (ex->sym->importedFrom && 0==unlinkObj(ex->sym->importedFrom->obj))
			/* nothing else to do */;
		if (ex->sym->importedFrom) {
			/* ex->sym.importedFrom must depend on a system module, skip to the next */
			p = ex->sym->importedFrom;
			do {
				if ( verbose & DEBUG_UNLINK ) {
					fprintf(debugf,"\n  skipping application dependeny; object '");
					printObjName(debugf,p->obj);
					fprintf(debugf,"'\n");
				}
				while ( (n=XREF_NEXT(p)) && 0 == unlinkObj(n->obj) )
					/* nothing else to do */;
			} while ( p = n ); /* reached a system module; skip */
		}
		if ( verbose & DEBUG_UNLINK )
			fprintf(debugf,"done.\n");
	}
	return 0;
}

static DepWalkAction	depwalkAction   = 0;
static void				*depwalkClosure = 0;
static int				depwalkMode     = 0;

/* walk all objects depending on this one */

#define DO_EXPORTS (depwalkMode & WALK_EXPORTS)

#define BUSY 		((ObjF)depwalk) /* just some address */
#define MATCH_ANY	((Lib)depwalk)	/* just some address */

static void
depwalk_rec(ObjF f, int depth)
{
register int	i;
register Xref ref;

//assert(depth < 55);

	if (depwalkAction)
		depwalkAction(f,depth,depwalkClosure);

	for ( i=0; i < (DO_EXPORTS ? f->nexports : f->nimports); i++ ) {
		for (ref = (DO_EXPORTS ? f->exports[i].sym->importedFrom : f->imports[i].sym->exportedBy);
			 ref;
			 ref = (DO_EXPORTS ? XREF_NEXT(ref) : 0 /* use only the first definition */) ) {

			assert( ref->obj != f );

			if ( !ref->obj->work ) {
				/* mark in use */
#ifdef NWORK
/*				fprintf(debugf,"Linking %s between %s and %s\n", ref->obj->name, f->name, f->work && f->work != BUSY ? f->work->name : "NIL");    */
				ref->obj->work = f->work;
				f->work        = ref->obj;
				assert( 0 == checkCircWorkList(f) );
#else
				ref->obj->work = f;
				if ( (depwalkMode & WALK_BUILD_LIST) ) {
					ref->obj->work1 = f->work1;
					f->work1	   = ref->obj;
				}
#endif
				depwalk_rec(ref->obj, depth+1);
				if ( ! (depwalkMode & WALK_BUILD_LIST) ) {
#ifdef NWORK
					f->work        = ref->obj->work;
					ref->obj->work = 0;
#else
					ref->obj->work = 0;
#endif
				}
			} /* else break circular dependency */
		}
	}
}


void
depwalk(ObjF f, DepWalkAction action, void *closure, int mode)
{
	assert(f->work == 0 );

	assert( !(depwalkMode & WALK_BUILD_LIST) );

	depwalkMode    = mode;
	depwalkAction  = (depwalkMode & WALK_BUILD_LIST) ? 0 : action;
	depwalkClosure = closure;

	f->work = BUSY;
	depwalk_rec(f, 0);

	if (depwalkMode & WALK_BUILD_LIST) {
		depwalkAction = action;
	} else {
		f->work = 0;
	}
}

void
workListRelease(ObjF f)
{
ObjF tmp;
#ifdef NWORK
	for (tmp = f; tmp && tmp!= BUSY; ) {
		tmp     = f->work;
		f->work = 0;
		f       = tmp;
	}
#else
	for (tmp = f; tmp; ) {
		tmp = f->work1;
		f->work1 = 0;
		f->work  = 0;
		f        = tmp;
	}
#endif
}

void
workListIterate(ObjF f, DepWalkAction action, void *closure)
{
int   depth = 0;
#ifdef NWORK
	while ( f && f != BUSY ) { 
		action(f, depth++, closure);
		f = f->work;
	}
#else
	while ( f ) {
		action(f, depth++, closure);
		f = f->work1;
	}
#endif
}

void
depwalkListRelease(ObjF f)
{
ObjF	tmp;
int		depth = 0;
	assert( (depwalkMode & WALK_BUILD_LIST) );
	if (depwalkAction)
		workListIterate(f, depwalkAction, depwalkClosure);
	workListRelease(f);
	depwalkMode = 0;
}


static void
symTraceAct(const void *pnode, const VISIT when, const int depth)
{
	if ( postorder == when || leaf == when ) {
		trackSym(logf, *(Sym*)pnode);
	}
}

int checkObjPtrs()
{
int		err=0;
ObjF	f;
	for (f=fileListHead; f; f=f->next) { 
		int ii;
		for (ii=0; ii<f->nexports; ii++) {
			if (f->exports[ii].obj != f) {
				fprintf(stderr,"%s %ith export obj pointer corrupted\n", f->name, ii);
				err++;
			}
		}
		for (ii=0; ii<f->nimports; ii++) {
			if (f->imports[ii].obj != f) {
				fprintf(stderr,"%s %ith import obj pointer corrupted\n", f->name, ii);
				err++;
			}
		}
	}
	return err;
}

typedef struct {
	ObjF	test;
	int		result;
} CheckArgRec, *CheckArg;

static void circCheckAction(ObjF f, int depth, void *closure)
{
CheckArg arg  = closure;
	if (depth > 0 && f == arg->test)
		arg->result = -1;
}

int checkCircWorkList(ObjF f)
{
CheckArgRec arg;
	arg.test   = f;
	arg.result = 0;
	workListIterate(f, circCheckAction, &arg);
	return arg.result;
}

void
depPrint(ObjF f, int d, void *closure)
{
DepPrintArg  arg  = (DepPrintArg)closure;
FILE		*feil = arg->file;

	if (d < arg->minDepth)
		return;

	if (!feil)
		feil = logf;

	if (arg->depthIndent >= 0)
		d <<= arg->depthIndent;
	else
		d = 0;
	d += arg->indent;
	while (d-- > 0)
		fputc(' ', feil);
	printObjName(feil, f);
	fputc('\n', feil);
}

static int
objcmp(const void *a, const void *b)
{
ObjF	obja=*(ObjF*)a;
ObjF	objb=*(ObjF*)b;
int		rval;

	if (rval = strcmp(obja->name, objb->name))
		return rval;

	if (MATCH_ANY == obja->lib  || MATCH_ANY == objb->lib)
		return 0;

	/* matching object names; compare libraries */
	if (obja->lib) {
		if (objb->lib)
			return strcmp(obja->lib->name, objb->lib->name);
		else
			return 1; /* a has library name, b has not b<a */
	}
	rval = objb->lib ? -1 : 0;

	return rval;
}

ObjF *
fileListBuildIndex()
{
ObjF *rval;
ObjF f;
int  i;

	assert( rval = malloc(numFiles * sizeof(*rval)) );
	for ( i=0, f = fileListHead; f; i++, f=f->next) {
		rval[i] = f;
	}
	qsort(rval, numFiles, sizeof(*rval), objcmp);
	return rval;
}

/* check for multiply define symbols in the link set of an object */
int
checkMultipleDefs(LinkSet s)
{
ObjF	f;
int		i;
Xref	r;
int		rval = 0;

	fprintf(logf,
			"Checking for multiply defined symbols in the %s link set:\n",
			s->name);

	for ( f = s->set; f; f=f->link.next ) {
		
		if ( BUSY == f->work )
			continue;

		for ( i = 0; i < f->nexports; i++ ) {
			r = f->exports[i].sym->exportedBy;


			if (XREF_NEXT(r)) {
				int isCommon = 0;
#ifdef TYPE
				isCommon = 'C' == TYPE(f->exports[i].sym);
#endif

				if ( !isCommon) {
					rval++;
					fprintf(logf,"WARNING: Name Clash Detected; symbol '%s'"
#ifdef TYPE
					   " (type '%c')"
#endif
					   " exported by multiple objects:\n",
						f->exports[i].sym->name
#ifdef TYPE
						, TYPE(f->exports[i].sym)
#endif
					);
				}
				while (r) {
					if (!isCommon) {
						fprintf(logf,"  in '");
						printObjName(logf,r->obj);
						fprintf(logf,"'%s\n", XREF_WEAK(r) ? " (WEAK [not implemented yet])" : "");
					}
					r->obj->work = BUSY;
					r = XREF_NEXT(r);
				}
			}
		}
	}

	for ( f = fileListHead; f; f=f->next )
		f->work = 0;

	fprintf(logf,"OK\n");

	return rval;
}

/* return the number of matches found.
 * A pointer to the first match in the index
 * array is returned in *pfound.
 *
 * 'name' is temporarily edited.
 */
int
fileListFind(char *name, ObjF **pfound)
{
char	*objn, *po, *pc;
ObjFRec	frec = {0};
ObjF	f    = &frec;
Lib		l;
ObjF	*found, *end;
int		rval = 0;

	if ( ! (objn=splitName(name,&po,&pc)) ) {
		return 0; /* ill-formed name */
	}

	f->name = objn;

	if (po && *name) {
		for (l=libListHead; l; l=l->next) {
			if ( !strcmp(l->name, name) ) {
				break;
			}
		}
		if (!l) {
			goto cleanup;
		}
		f->lib = l;
	} else  {
		f->lib = MATCH_ANY;
	}

	if ( ! (found = bsearch(&f, fileListIndex, numFiles, sizeof(f), objcmp)) )
		goto cleanup;

	/* find all matches */
	end = found;
	while ( ++end < fileListIndex + numFiles && !objcmp(&f,end) )
		/* nothing else to do */;

	while (--found >= fileListIndex && !objcmp(&f, found))
		/* nothing else to do */;

	found++;

	if (pfound)
		*pfound = found;

	rval = end-found;

cleanup:
	if (po) {
		*po = '[';
		*pc = ']';
	}
	return rval;
}

int
removeObjs(char *fname)
{
FILE *remf;
char buf[MAXBUF+1];
int  got,i;
int  line;
ObjF *pobj;

	buf[MAXBUF] = 'X'; /* tag end of buffer */

	if ( ! (remf=fopen(fname,"r")) ) {
		perror("opening removal list file");
		return -1;
	}

	fprintf(logf,
			"Processing list of files ('%s') to unlink from %s link set\n",
			fname,
			optionalLinkSet.name);

	line = 0;
	while ( EOF != (got=fscanf(remf,THENMFMT,buf)) ) {
		line++;
		if (!buf[MAXBUF]) {
			fprintf(stderr,"Buffer overflow in %s (line %i)\n",
							fname,
							line);
			fclose(remf);
			return -1;
		}

		if (got<1)
			continue;

		got = fileListFind(buf, &pobj);

		if ( 0 == got ) {
			fprintf(stderr,"Object '%s' not found, skipping...", buf);
		} else if (got > 1) {
			fprintf(stderr,"Multiple occurrences of '%s':\n",buf);
			for (i=0; i<got; i++) {
				fputc(' ',stderr);
				fputc(' ',stderr);
				printObjName(stderr,pobj[i]);
				fputc('\n',stderr);
			}
			fprintf(stderr,"please be more specific; skipping '%s'\n",buf);
		} else  {
			if (unlinkObj(*pobj)) {
				fprintf(stderr,"Object '%s' couldn't be removed; probably it's needed by the application", buf);
			}
		}
		fputc('\n',stderr);
	}

	fclose(remf);
	return 0;
}

static int
writeLinkSet(FILE *feil, LinkSet s, char *title)
{
ObjF	f = s->set;
int		n;

	if ( !f )
		return 0;

	if (title)
		fprintf(feil,"/* ----- %s Link Set ----- */\n\n", title);

	for ( ; f; f = f->link.next ) {
		fprintf(feil,"/* "); printObjName(feil,f); fprintf(feil,": */\n");
		for ( n = 0; n < f->nexports; n++ ) {
			fprintf(feil,"EXTERN( %s )\n", f->exports[n].sym->name);
		}
	}
}

/* Generate a linker script with external references to enforce linking the
 * application and optional link sets
 */
int
writeScript(FILE *feil, int optionalOnly)
{
	if ( !optionalOnly ) {
		writeLinkSet(feil, &appLinkSet, "Application");
		fputc('\n',feil);
	}

	writeLinkSet(feil, &optionalLinkSet, "Optional");
	return 0;
}

static void 
usage(char *nm)
{
char *strip = strrchr(nm,'/');
	if (strip)
		nm = strip+1;
	fprintf(stderr,"\nUsage: %s [-dfhilmqsu] [-r removal_list] [-o log_file] [-e script_file] [nm_files]\n\n", nm);
	fprintf(stderr,"   Object file dependency analysis; the input files must be\n");
	fprintf(stderr,"   created with 'nm -g -fposix'.\n\n");
	fprintf(stderr,"(This is ldep $Revision$ by Till Straumann <strauman@slac.stanford.edu>)\n\n");
	fprintf(stderr,"   Input:\n");
	fprintf(stderr,"           If no 'nm_files' are given, 'stdin' is used. The first 'nm_file' is\n");
	fprintf(stderr,"           special: it lists MANDATORY objects/symbols ('application files')\n");
	fprintf(stderr,"           objects added by the other 'nm_files' are 'optional' unless a mandatory\n");
	fprintf(stderr,"           object depends on an optional object. In this case, the latter becomes\n");
	fprintf(stderr,"           mandatory as well.\n\n");

	fprintf(stderr,"   Options:\n");
	fprintf(stderr,"     -d:   show all module dependencies (huge amounts of data! -- use '-l', '-u')\n");
	fprintf(stderr,"     -e:   on success, generate a linker script 'script_file' with EXTERN statements\n");
	fprintf(stderr,"     -f:   be less paranoid when scanning symbols: accept 'local symbols' (map all\n");
	fprintf(stderr,"           types to upper-case) and assume unrecognized symbol types ('?') are 'U'\n");
	fprintf(stderr,"     -h:   print this message.\n");
	fprintf(stderr,"     -i:   enter interactive mode\n");
	fprintf(stderr,"     -l:   log info about the linking process\n");
	fprintf(stderr,"     -m:   check for symbols defined in multiple files\n");
	fprintf(stderr,"     -o:   log messages to 'log_file' instead of 'stdout' (default)\n");
	fprintf(stderr,"     -q:   quiet; just build database and do basic checks\n");
	fprintf(stderr,"     -r:   remove a list of objects from the link - name them, one per line, in\n");
    fprintf(stderr,"           the file 'removal_list'\n");
	fprintf(stderr,"           NOTE: if a mandatory object depends on an object to be removed, removal\n");
	fprintf(stderr,"                 is rejected.\n");
	fprintf(stderr,"     -s:   show all symbol info (huge amounts of data! -- use '-l', '-u')\n");
	fprintf(stderr,"     -u:   log info about the unlinking process\n");
}

int
interactive(FILE *feil)
{
Sym		*found;
ObjF	*f;
char	buf[MAXBUF+1];
int		len, nf, i, choice;
SymRec	sym = {0};

	sym.name = buf;

	buf[0]=0;

	buf[MAXBUF]='0';

	do {
		if ( MAXBUF == (len=strlen(buf)) ) {
			fprintf(stderr,"Line buffer overflow in interactive mode\n");
			return -1;
		}

		if ( !*buf || '\n'==*buf ) {
			fputc('\n',feil);
			fprintf(feil, "Query database (enter single '.' to quit) for\n");
			fprintf(feil, " A) Symbols, e.g. 'printf'\n");
			fprintf(feil, " B) Objects, e.g. '[printf.o]', 'libc.a[printf.o]'\n\n");
		} else {
			buf[--len]=0; /* strip trailing '\n' */
			if ( ']' == buf[len-1] ) {
				nf = fileListFind(buf, &f);

				if ( !nf ) {
					fprintf(feil,"object '%s' not found, try again.\n", buf);
				} else {
					choice = 0;
					if ( nf > 1 ) {
						fprintf(feil,"multiple instances found, make a choice:\n");
						for (i = 0; i<nf; i++) {
							fprintf(feil,"%i) - ",i); printObjName(feil, f[i]); fputc('\n', feil);
						}

						while (    !fgets( buf, MAXBUF, stdin) ||
								 1 !=sscanf(buf,"%i",&choice)  ||
								 choice < 0                    ||
								 choice >= nf ) {

								if ( !strcmp(".\n",buf) )
									return 0;

								fprintf(feil, "\nInvalid Choice, ");
								
								if (!*buf) {
									fprintf(feil,"bailing out\n");
									return -1;
								}

								fprintf(feil,"try again\n");
								*buf = 0;
						}


					}
					trackObj(feil, f[choice]);
				}
			} else {
				found = (Sym*) tfind(&sym, &symTbl, symcmp);

				if ( !found ) {
					fprintf(feil,"Symbol '%s' not found, try again\n", sym.name);
				} else {
					trackSym(feil, *found);
				}
			}
		}
	} while ( fgets(buf, MAXBUF, stdin) && *buf && strcmp(buf,".\n") );
}

int
main(int argc, char **argv)
{
FILE	*feil = stdin;
FILE	*scrf = 0;
char	*scrn = 0;
ObjF	f, lastAppObj=0; 
LinkSet	linkSet;
int		i,nfile,ch;
int		quiet = 0;
int		showSyms = 0;
int		showDeps = 0;
int		multipleDefs = 0;
char	*removalList = 0;
int		interActive  = 0;

	logf = stdout;

	while ( (ch=getopt(argc, argv, "qhifsdmlur:o:e:")) >= 0 ) {
		switch (ch) { 
			default: fprintf(stderr, "Unknown option '%c'\n",ch);
					 exit(1);

			case 'h':
				usage(argv[0]);
				exit(0);

			case 'l': verbose |= DEBUG_LINK;
			break;
			case 'u': verbose |= DEBUG_UNLINK;
			break;
			case 'd': showDeps     = 1;
			break;
			case 'f': force        = 1;
			break;
			case 'i': interActive  = 1;
			break;
			case 's': showSyms     = 1;
			break;
			case 'q': quiet        = 1;
			break;
			case 'r': removalList  = optarg;
			break;
			case 'm': multipleDefs = 1;
			break;
			case 'o':
				if ( !(logf=fopen(optarg,"w")) ) {
					perror("opening log file");
					exit(1);
				}
			break;
			case 'e': scrn = optarg;
			break;
		}
	}

	debugf = logf;

	nfile = optind;

	do {
		char *nm = nfile < argc ? argv[nfile] : "<stdin>";
		if ( nfile < argc && !(feil=fopen(nm,"r")) ) {
			perror("opening file");
			exit(1);
		}
		if (scan_file(feil,nm)) {
			fprintf(stderr,"Error scanning %s\n",nm); 
			exit(1);
		}
		/* the first file we scan contains the application's
		 * mandatory file set
		 */
		if ( !lastAppObj )
			lastAppObj = fileListTail;
	} while (++nfile < argc);

	gatherDanglingUndefs();

	fileListIndex = fileListBuildIndex();

	fprintf(logf,"Looking for UNDEFINED symbols:\n");
	for (i=0; i<fileListHead->nexports; i++) {
#if 0
		trackSym(logf, fileListHead->exports[i].sym);
#else
		fprintf(logf," - '%s'\n",fileListHead->exports[i].sym->name);
#endif
	}
	fprintf(logf,"done\n");

	assert( 0 == checkObjPtrs() );

	for ( f=fileListFirst(), linkSet = &appLinkSet ; f; f=f->next) {
		if (!f->link.anchor) {
			f->link.anchor = linkSet;
			linkObj(f, 0);
		}
		if ( f==lastAppObj )
			linkSet = &optionalLinkSet;	
	}

	if (quiet) {
		fprintf(logf,"OK, that's it for now\n");
		exit(0);
	}

	if (showSyms)
		twalk(symTbl, symTraceAct);

	if (showDeps) {
			for (f=fileListFirst(); f; f=f->next) {
			DepPrintArgRec arg;
				arg.minDepth    =  0;
				arg.indent      = -4;
				arg.depthIndent = 2;
				arg.file		= logf;
#if 0
			/* this recursion can become VERY deep */
			fprintf(logf,"\n\nDependencies ON object: ");
			depwalk(f, depPrint, (void*)&arg, WALK_EXPORTS);
#endif
			fprintf(logf,"\nFlat dependency list for objects requiring: %s\n", f->name);
			arg.indent      = 0;
			arg.depthIndent = -1;
			depwalk(f, depPrint, (void*)&arg, WALK_EXPORTS | WALK_BUILD_LIST);
			depwalkListRelease(f);
		}
	}

	fprintf(logf,"Removing undefined symbols\n");
	unlinkUndefs();

	if (removalList) {
		if (removeObjs(removalList))
			exit(1);
	}

	if (multipleDefs) {
		checkMultipleDefs(&appLinkSet);
		checkMultipleDefs(&optionalLinkSet);
	}

	if (interActive) {
		interactive(stderr);
	}

	assert( 0 == checkObjPtrs() );

	if ( scrn ) {
		fprintf(logf,"Writing linker script to '%s'...", scrn);
		if ( !(scrf = fopen(scrn,"w")) ) {
			perror("opening script file");
			fprintf(logf,"opening file failed.\n");
			exit (1);
		}
		writeScript(scrf,0);
		fclose(scrf);
		fprintf(logf,"done.\n");
	}

	return 0;
}
