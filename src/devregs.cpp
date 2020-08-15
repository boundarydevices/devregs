/*
 * devregs - tool to display and modify a device's registers at runtime
 *
 * Use cases:
 *
 *	devregs
 *		- display all registers
 *
 *	devregs register
 *		- display all registers matching register (strcasestr)
 *
 *	devregs register.field
 *		- display all registers matching register (strcasestr)
 *		- also break out specified field
 *
 *	devregs register value
 *		- set register to specified value (must match single register)
 *
 *	devregs register.field value
 *		- set register field to specified value (read/modify/write)
 *
 * Registers may be specified by name or 0xADDRESS. If specified by name, all
 * registers containing the pattern are considered. If multiple registers 
 * match on a write request (2-parameter use cases), no write will be made.
 *
 * fields may be specified by name or bit numbers of the form "start[-end]"
 *
 * (c) Copyright 2010 by Boundary Devices under GPLv2
 *
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>

typedef off_t phys_addr_t;

static bool word_access = false ;
static int unsigned cpu_in_params = 0;
static bool fancy_color_mode = false;
static bool stdout_tty = isatty(STDOUT_FILENO);

struct fieldDescription_t {
	char const 		   *name ;
	unsigned    		   startbit ;
	unsigned    		   bitcount ;
	struct fieldDescription_t *next ;
};

struct registerDescription_t {
	char const 		*name ;
	fieldDescription_t 	*fields ;
};

struct reglist_t {
	phys_addr_t 			address ;
	unsigned		 	 width ; // # bytes in register
	struct registerDescription_t	*reg ;
	struct fieldDescription_t	*fields ;
	struct reglist_t		*next ;
};

struct	fieldSet_t {
	char const 			*name ;
	struct fieldDescription_t	*fields ;
        struct	fieldSet_t 		*next ;
};

static struct fieldSet_t *fieldsets = 0 ;

/* 
 * strips comments as well as skipping leading spaces
 */
char *skipSpaces(char *buf){
	char *comment = strchr(buf,'#');
	if (comment)
		*comment = '\0' ;
	comment = strstr(buf,"//");
	if (comment)
		*comment = 0 ;
	while( *buf ){
		if( isprint(*buf) && (' ' != *buf) )
			break;
		buf++ ;
	}
	return buf ;
}

static void trimCtrl(char *buf){
	char *tail = buf+strlen(buf);
	// trim trailing <CR> if needed
	while( tail > buf ){
		--tail ;
		if( iscntrl(*tail) ){
			*tail = '\0' ;
		} else
			break;
	}
}

static bool parseBits(char const *bitspec, unsigned &start, unsigned &count)
{
	char *end ;
	unsigned startbit = strtoul(bitspec,&end,0);
	if( (31 >= startbit)
	    &&
	    ( ('\0' == *end)
	      ||
	      ('-' == *end) ) ){
		unsigned endbit ;
		if( '-' == *end ){
			endbit = strtoul(end+1,&end,0);
			if('\0' != *end){
				endbit = ~startbit ;
			}
		} else {
			endbit = startbit ;
		}
		if(endbit<startbit) {
			endbit ^= startbit ;
			startbit ^= endbit ;
			endbit  ^= startbit ;
		}
		unsigned const bitcount = endbit-startbit+1 ;
		if( bitcount <= (32-startbit) ){
			start = startbit ;
			count = bitcount ;
			return true ;
		} else
			fprintf(stderr, "Invalid bitspec '%s'. Use form 'start-end' in decimal (%u,%u,%u)\n", bitspec,startbit,endbit,bitcount );
	} else
		fprintf(stderr, "Invalid field '%s'. Use form 'start-end' in decimal (%u,%x)\n", bitspec,startbit,*end );

	return false ;
}

static struct fieldDescription_t *parseFields
	( struct reglist_t const *regs,
	  char const *fieldname )
{
	if(isdigit(*fieldname)){
		unsigned start, count ;
		if (parseBits(fieldname,start,count)){
			fieldDescription_t *f = new fieldDescription_t ;
			f->name = fieldname ;
			f->startbit = start ;
			f->bitcount = count ;
			f->next = 0 ;
			return f ;
		}
	} else if( regs ){
                struct fieldDescription_t *head = 0 ;
		struct fieldDescription_t *tail = 0 ;
		while( regs ){
			if( regs->reg ){
				struct fieldDescription_t *f = regs->reg->fields ;
				while( f ){
					if( 0 == strcasecmp(f->name,fieldname) ){
						struct fieldDescription_t *newf = new struct fieldDescription_t ;
						*newf = *f ;
						newf->next = 0 ;
						tail = newf ;
						if( 0 == head )
							head = newf ;
					}
					f = f->next ;
				}
			}
			regs = regs->next ;
		}
		return head ;
	} else {
		fprintf(stderr, "Can't parse named fields without matching registers\n" );
	}
	return 0 ;
}

/*
 *	- Outer loop determines which type of line we're dealing with
 *	based on the first character:
 *		A-Za-z_		- Register:	Name	0xADDRESS[.w|.l|.b]
 *		:		- Field		:fieldname:startbit[-stopbit]
 *		/		- Field set	/Fieldsetname
 *
 *	state field is used to determine whether a field will be added to the
 *	most recent register or fieldset.
 */
enum ftState {
	FT_UNKNOWN	= -1,
	FT_REGISTER	= 0,
	FT_FIELDSET	= 1
};

static char const *getDataPath(unsigned cpu) {
	switch (cpu & 0xff000) {
		case 0x63000:
			return "/etc/devregs_imx6q.dat" ;
		case 0x61000:
			return "/etc/devregs_imx6dls.dat" ;
		case 0x53000:
			return "/etc/devregs_imx53.dat" ;
	}
	switch (cpu) {
	case 0x10:
		return "/etc/devregs_imx6q.dat";
	case 0x51:
	case 0x5:
		return "/etc/devregs_imx51.dat";
	case 0x7:
		return "/etc/devregs_imx7d.dat";
	case 0x81:
		return "/etc/devregs_imx8mq.dat";
	case 0x82:
		return "/etc/devregs_imx8mm.dat";
	default:
		printf("unsupported CPU type: %x\n", cpu);
	}
	return "/etc/devregs.dat" ;
}

static struct reglist_t const *registerDefs(unsigned cputype = 0){
	static struct reglist_t *regs = 0 ;
	if( 0 == regs ){
		struct reglist_t *head = 0, *tail = 0 ;
		const char *filename = getDataPath(cputype);
		FILE *fDefs = fopen(filename, "rt");
		if( fDefs ){
                        enum ftState state = FT_UNKNOWN ;
			char inBuf[256];
			int lineNum = 0 ;

//			printf("Using %s\n", filename);
			while( fgets(inBuf,sizeof(inBuf),fDefs) ){
				lineNum++ ;
				// skip unprintables
                                char *next = skipSpaces(inBuf);
				if( *next && ('#' != *next) ){
					trimCtrl(next);
				} // not blank or comment
				if(isalpha(*next) || ('_' == *next)){
					char *start = next++ ;
					while(isalnum(*next) || ('_' == *next)){
						next++ ;
					}
					if(isspace(*next)){
						char *end=next-1 ;
						next=skipSpaces(next);
						if(isxdigit(*next)){
							char *addrEnd ;
							phys_addr_t addr = (phys_addr_t )strtoul(next,&addrEnd,16);
							unsigned width = 4 ;
							if( addrEnd && ('.' == *addrEnd) ){
								char widthchar = tolower(addrEnd[1]);
								if('w' == widthchar) {
									width = 2 ;
								} else if( 'b' == widthchar) {
									width = 1 ;
								} else if( 'l' == widthchar) {
									width = 4 ;
								}
								else {
									fprintf(stderr, "Invalid width char %c on line number %u\n", widthchar, lineNum);
									continue;
								}
								addrEnd = addrEnd+2 ;
							}
							if( addrEnd && ('\0'==*addrEnd)){
								unsigned namelen = end-start+1 ;
								char *name = (char *)malloc(namelen+1);
								memcpy(name,start,namelen);
								name[namelen] = '\0' ;
                                                                struct reglist_t *newone = new reglist_t ;
								newone->address=addr ;
								newone->width = width ;
								newone->reg = new registerDescription_t ;
								newone->reg->name = name ;
								newone->reg->fields = newone->fields = 0 ;
								if(tail){
									tail->next = newone ;
								} else
									head = newone ;
								tail = newone ;
                                                                state = FT_REGISTER ;
//								printf( "%s: 0x%08lx, width %u\n", newone->reg->name, newone->address, newone->width);
								continue;
							}
							else
								fprintf(stderr, "expecting end of addr, not %c\n", addrEnd ? *addrEnd : '?' );
						}
						else
							fprintf(stderr, "expecting hex digit, not %02x\n", (unsigned char)*next );
					}
					fprintf(stderr, "%s: syntax error on line %u <%s>\n", filename, lineNum,next );
				} else if((':' == *next) && (FT_UNKNOWN != state)) {
                                        next=skipSpaces(next+1);
					char *start = next++ ;
					while(isalnum(*next) || ('_' == *next)){
						next++ ;
					}
					unsigned nameLen = next-start ;
					char *name = new char [nameLen+1];
					memcpy(name,start,nameLen);
					name[nameLen] = 0 ;
					if( ':' == *next ){
						struct fieldDescription_t *field = parseFields(tail,next+1);
						if(field){
							field->name = name ;
							if (FT_REGISTER == state) {
								field->next = tail->fields ;
								tail->fields = field ;
							} else {
								field->next = fieldsets->fields ;
								fieldsets->fields = field ;
							}
						} else 
                                                        fprintf( stderr, "error parsing field at line %u\n", lineNum );
					} else if (('/' == *next) && (FT_REGISTER == state)) {
						struct fieldSet_t const *fs = fieldsets ;
						while (fs) {
							if (0 == strcmp(fs->name,name))
								break;
							fs = fs->next ;
						}
						if (fs) {
							if (tail->fields) {
                                                                struct fieldDescription_t *back = tail->fields ;
                                                                struct fieldDescription_t *front = back ;
								while(front) {
									back = front ;
									front = back->next ;
								}
								back->next = fs->fields ;
							} else
                                                                tail->fields = fs->fields ;
							state = FT_UNKNOWN ; /* don't allow fields to be added */
						}
					} else {
						fprintf( stderr, "missing field separator at line %u\n", lineNum );
					}
				} else if ('/' == *next) {
					char *start = ++next ;
					while(isalnum(*next) || ('_' == *next)){
						next++ ;
					}
					if ((start < next) && (isspace(*next) || ('\0'==*next))) {
						*next = '\0' ;
                                                struct	fieldSet_t  *fs = (struct fieldSet_t *)malloc(sizeof(struct fieldSet_t ));
						fs->name = strdup(start);
						fs->fields = 0 ;
						fs->next = fieldsets ;
						fieldsets = fs ;
						state = FT_FIELDSET ;
					} else
						fprintf(stderr,"Invalid fieldset name %s\n",start-1);
				} else if (*next && ('#' != *next)) {
					fprintf(stderr, "Unrecognized line <%s> at %u\n", next, lineNum );
				}
			}
			fclose(fDefs);
			regs = head ;
		} else 
			perror(filename);
	}
	return regs ;
}

static struct reglist_t const *parseRegisterSpec(char const *regname)
{
	char const c = *regname ;

	if(isalpha(c) || ('_' == c)){
                struct reglist_t *out = 0 ;
                struct reglist_t const *defs = registerDefs();
		char *regPart = strdup(regname);
		char *fieldPart = strchr(regPart,'.');
		bool widthspec = false ;
		unsigned fieldLen = 0 ;

		if (0 == fieldPart) {
			fieldPart = strchr(regPart,':');
			widthspec = false ;
		}
		else
			widthspec = true ;
		if (fieldPart) {
			*fieldPart++ = '\0' ;
			fieldLen = strlen(fieldPart);
		}
		unsigned const nameLen = strlen(regname);
		while(defs){
                        if( 0 == strncasecmp(regPart,defs->reg->name,nameLen) ) {
				struct reglist_t *newOne = new struct reglist_t ;
				memcpy(newOne,defs,sizeof(*newOne));
				if (fieldPart) {
					newOne->fields = 0 ;
					if (isdigit(*fieldPart)) {
						unsigned start, count ;
						if (parseBits(fieldPart,start,count)) {
							fieldDescription_t *newf = new struct fieldDescription_t ;
							newf->name = fieldPart ;
							newf->startbit = start ;
							newf->bitcount = count ;
							newf->next = 0 ;
							newOne->fields = newf ;
						}
						else
							return 0 ;
					} else {
						fieldDescription_t *rhs = defs->fields ;
						while (rhs) {
							if( 0 == strcasecmp(fieldPart,rhs->name) ) {
								fieldDescription_t *newf = new struct fieldDescription_t ;
								memcpy(newf,rhs,sizeof(*newf));
								newf->next = newOne->fields ;
								newOne->fields = newf ;
							}
							rhs = rhs->next ;
						}
					} // search for named fields
				} // only copy specified field
				newOne->next = out ;
				out = newOne ;
			}
			defs = defs->next ;
		}
		free(regPart);
		return out ;
	} else if(isdigit(c)){
		char *end ;
		phys_addr_t address = (phys_addr_t)strtoul(regname,&end,16);
		if( (0 == *end) || (':' == *end) || ('.' == *end) ){
                        struct fieldDescription_t *field = 0 ;
			unsigned start, count ;
			struct reglist_t *out = 0 ;
			struct reglist_t const *defs = registerDefs();
			unsigned const nameLen = strlen(regname);
			while(defs){
				if( defs->address == address ) {
					out = new struct reglist_t ;
					memcpy(out,defs,sizeof(*out));
					out->next = 0 ;
					out->fields = field ;
					break;
				}
				defs = defs->next ;
			}

			if (':' == *end) {
				unsigned start, count ;
				if (parseBits(end+1,start,count)) {
					field = new struct fieldDescription_t ;
					field->name = end+1 ;
					field->startbit = start ;
					field->bitcount = count ;
					field->next = 0 ;
				}
			}
			struct fieldDescription_t *fields = 0 ;
			unsigned width = 4 ;
			if( '.' == *end ){
				char widthchar=tolower(end[1]);
				if ('w' == widthchar) {
					width = 2 ;
				} else if ('b' == widthchar) {
					width = 1 ;
				} else if ('l' == widthchar) {
					width = 4 ;
				} else {
					fprintf( stderr, "Invalid width char <%c>\n", widthchar);
				}
			}
			if( 0 == out ){
                                out = new struct reglist_t ;
				out->address = address ;
				out->width = width ;
				out->reg = 0 ;
				out->fields = fields ;
				out->next = 0 ;
			}
			return out ;
		} else {
			fprintf( stderr, "Invalid register name or value '%s'. Use name or 0xHEX\n", regname );
		}
	} else {
		fprintf( stderr, "Invalid register name or value '%s'. Use name or 0xHEX\n", regname );
	}
	return 0 ;
}

static int getFd(void){
	static int fd = -1 ;
	if( 0 > fd ){
		fd = open("/dev/mem", O_RDWR | O_SYNC);
		if (fd<0) {
			perror("/dev/mem");
			exit(1);
		}
	}
	return fd ;
}

#define MAP_SIZE 4096
#define MAP_MASK ( MAP_SIZE - 1 )

static unsigned volatile *getReg(phys_addr_t addr){
	static void *map = 0 ;
	static phys_addr_t prevPage = -1U ;
	unsigned offs = addr & MAP_MASK ;
	phys_addr_t page = addr - offs;
	if( page != prevPage ){
		if( map ){
		   munmap(map,MAP_SIZE);
		}
		map = mmap(0, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, getFd(), page );
		if( MAP_FAILED == map ){
			perror("mmap");
			exit(1);
		}
		prevPage = page ;
	}
	return (unsigned volatile *)((char *)map+offs);
}

static unsigned fieldVal(struct fieldDescription_t *f, unsigned v)
{
	v >>= f->startbit ;
	v &= (1<<f->bitcount)-1 ;
	return v ;
}

#define RED	"\e[0;31m"
#define GREEN	"\e[1;32m"
#define BLUE	"\e[1;34m"
#define YELLOW	"\e[1;33m"
#define CYAN	"\e[0;36m"
#define RST	"\e[1;0m"
#define COL(_color)	(stdout_tty && fancy_color_mode ? _color : "")

static void showReg(struct reglist_t const *reg)
{
	unsigned rv ; 
        unsigned volatile *regPtr = getReg(reg->address);
	if( 2 == reg->width ) {
		unsigned short volatile *p = (unsigned short volatile *)regPtr ;
		rv = *p ;
		printf( "%s:0x%08lx\t=0x%04x\n", reg->reg ? reg->reg->name : "", reg->address, rv );
	} else if( 4 == reg->width ) {
		unsigned volatile *p = regPtr ;
		rv = *p ;
		printf( "%s:0x%08lx\t=0x%08x\n", reg->reg ? reg->reg->name : "", reg->address, rv );
	} else if( 1 == reg->width ) {
		unsigned char volatile *p = (unsigned char volatile *)regPtr ;
		rv = *p ;
		printf( "%s:0x%08lx\t=0x%02x\n", reg->reg ? reg->reg->name : "", reg->address, rv );
	}
	else {
		fprintf(stderr, "Unsupported width in register %s\n", reg->reg->name);
		return ;
	}
	fflush(stdout);
	struct fieldDescription_t *f = reg->fields ;
	while(f){
		printf("\t%s%-16s%s", COL(CYAN), f->name, COL(RST));
		printf("\t%s%2u-%2u%s", COL(BLUE),  f->startbit, f->startbit+f->bitcount-1, COL(RST));
		printf("\t=%s0x%x%s",  fieldVal(f,rv) ? COL(YELLOW) : "", fieldVal(f,rv), COL(RST));
		if (fancy_color_mode) {
			int len = f->bitcount;
			unsigned b = fieldVal(f,rv);
			printf("\t");
			while (--len >= 0) {
				if ((b >> len) & 1)
					printf("%s%u%s", COL(GREEN), 1, COL(RST));
				else
					printf("%s%u%s", COL(RED), 0, COL(RST));
			}
		}
		printf("\n");
		fflush(stdout);
		f=f->next ;
	}
}

static void putReg(struct reglist_t const *reg,unsigned value){
	unsigned shift = 0 ;
	unsigned mask = 0xffffffff ;
	if (reg->fields) {
		// Only single field allowed
		if (0 == reg->fields->next) {
			shift = reg->fields->startbit ;
			mask = ((1<<reg->fields->bitcount)-1)<<shift ;
		} else {
			fprintf(stderr, "More than one field matched %s\n", reg->reg->name);
			return ;
		}
	}
	unsigned maxValue = mask >> shift ;
	if (value > maxValue) {
		fprintf(stderr, "Value 0x%x exceeds max 0x%x for register %s\n", value, maxValue, reg->reg->name);
		return ;
	}
	if( 1 == reg->width ){
		unsigned char volatile * const rv = (unsigned char volatile *)getReg(reg->address);
		value = (*rv&~mask) | ((value<<shift)&mask);
		printf( "%s:0x%08lx == 0x%02x...", reg->reg ? reg->reg->name : "", reg->address, *rv );
		*rv = value ;
	} else if( 2 == reg->width ){
		unsigned short volatile * const rv = (unsigned short volatile *)getReg(reg->address);
		value = (*rv&~mask) | ((value<<shift)&mask);
		printf( "%s:0x%08lx == 0x%04x...", reg->reg ? reg->reg->name : "", reg->address, *rv );
		*rv = value ;
	} else {
		unsigned volatile * const rv = getReg(reg->address);
		value = (*rv&~mask) | ((value<<shift)&mask);
		printf( "%s:0x%08lx == 0x%08x...", reg->reg ? reg->reg->name : "", reg->address, *rv );
		*rv = value ;
	}
	printf( "0x%08x\n", value );
}

static void printUsage(void) {
	printf("Usage: devregs [-w] [-c CPUNAME]\n");
	puts("  -w   Using word access\n"
		 "  -f fancy color mode (-ff to force, for e.g. pipe to less -r)\n"
		 "  -c CPUNAME in case the revision is not readable in /proc/cpuinfo fixit manually with :\n"
			"\timx8mm\n"
			"\timx8mq\n"
			"\timx7d\n"
			"\timx6q\n"
			"\timx6dls\n"
			"\timx53\n"
		 );
	exit(1);
}

static void parseArgs( int &argc, char const **argv )
{
	int arg = 1;

	while (arg < argc) {
		char const *p = argv[arg];
		if ('-' == *p++ ) {
			unsigned skip = 1;
			if ('w' == tolower(*p)) {
				word_access = true ;
				printf("Using word access\n" );
			} else if ('f' == tolower(*p)) {
				fancy_color_mode = true ;
				printf("Using fancy color mode\n");
				if(!strcmp(argv[arg], "-ff"))
					printf("Forcing fancy color mode\n");
					stdout_tty = true;
			} else if ('c' == tolower(*p)) {
				p = argv[arg + skip];
				if (!p){
					fprintf(stderr,"Do not forget to specify CPUNAME\n");
					printUsage();
				}
				if(!strcmp(p, "imx6q")){
					skip++;
					printf("Fixing cpu to %s\n","imx6q");
					cpu_in_params = 0x63000;
				} else if(!strcmp(p, "imx6dls")) {
					skip++;
					printf("Fixing cpu to %s\n","imx6dls");
					cpu_in_params = 0x61000;
				} else if(!strcmp(p, "imx53")) {
					skip++;
					printf("Fixing cpu to %s\n","imx53");
					cpu_in_params = 0x53000;
				} else if(!strcmp(p, "imx7d")) {
					skip++;
					printf("Fixing cpu to %s\n","imx7d");
					cpu_in_params = 0x7;
				} else if(!strcmp(p, "imx8mq")) {
					skip++;
					printf("Fixing cpu to %s\n","imx8mq");
					cpu_in_params = 0x81;
				} else if(!strcmp(p, "imx8mm")) {
					skip++;
					printf("Fixing cpu to %s\n","imx8mm");
					cpu_in_params = 0x82;
				} else {
					printf("Unable to interpret cpu name %s\n", p);
					printUsage();
				}
			} else{
				printf( "unknown option %s\n", p);
				printUsage();
			}

			// pull from argument list
			argc -= skip;
			for (int j = arg; j < argc; j++) {
				argv[j] = argv[j + skip];
			}
			continue;
		}
		arg++;
	}
}


static int get_rev(char * inBuf, const char* match, unsigned *pcpu)
{
	int rc = -1;
	char *rev = strstr(inBuf, match);

//	printf("%s\n", inBuf);
	if (rev && (0 != (rev=strchr(rev, ':')))) {
		char *next = rev + 2;
		unsigned cpu = 0;
		while (isxdigit(*next)) {
			cpu <<= 4 ;
			unsigned char c = toupper(*next++);
			if (('0' <= c)&&('9' >= c)) {
				cpu |= (c-'0');
			} else {
				cpu |= (10+(c-'A'));
			}
		}
		*pcpu = cpu;
		rc = 0;
	}
	return rc;
}

static int getcpu(unsigned &cpu, const char *path) {
	int processor_cnt = 0;
	cpu = 0 ;
	FILE *fIn = fopen(path, "r");
	if (fIn) {
		char inBuf[512];
		while (fgets(inBuf,sizeof(inBuf),fIn)) {
			if (strstr(inBuf, "i.MX7")) {
				cpu = 0x7;
				break;
			}
			if (strstr(inBuf, "i.MX51")) {
				cpu = 0x51;
				break;
			}
			if (strstr(inBuf, "i.MX8MQ")) {
				cpu = 0x81;
				break;
			}
			if (strstr(inBuf, "i.MX8MM")) {
				cpu = 0x82;
				break;
			}
			if (strstr(inBuf, "i.MX8MN")) {
				cpu = 0x82;
				break;
			}
			if (!get_rev(inBuf, "Revision", &cpu))
				if (cpu != 0x10)
					break;
			if (!get_rev(inBuf, "revision", &cpu))
				if ((cpu != 0x10) && (cpu != 5))
					break;
			if (strstr(inBuf, "processor"))
				processor_cnt++;
		}
		fclose(fIn);
	}
	if ((cpu == 0x10) || !cpu) {
		if ((processor_cnt == 1) || (processor_cnt == 2))
			cpu = 0x61000;
		else if (processor_cnt == 4)
			cpu = 0x63000;
	}
	return (0 != cpu);
}

int main(int argc, char const **argv)
{
	unsigned cpu ;
	unsigned parse_arguments = 1;

	parseArgs(argc,argv);
	if (!cpu_in_params && !getcpu(cpu, "/sys/devices/soc0/soc_id") &&
	    !getcpu(cpu, "/proc/cpuinfo")) {
		fprintf(stderr, "Error reading CPU type\n");
		fprintf(stderr, "Try to fixit using -c option\n");
		return -1 ;
	}
	if (cpu_in_params)
		cpu = cpu_in_params;
	//printf( "CPU type is 0x%x\n", cpu);
	registerDefs(cpu);
	if( 1 == argc ){
                struct reglist_t const *defs = registerDefs();
		while(defs){
                        showReg(defs);
			defs = defs->next ;
		}
	} else {
                struct reglist_t const *regs = parseRegisterSpec(argv[parse_arguments]);
		if( regs ){
			if( 2 == (argc-parse_arguments+1) ){
				while( regs ){
					showReg(regs);
					regs = regs->next ;
				}
			} else {
				char *end ;
				unsigned value = strtoul(argv[1+parse_arguments],&end,16);
				if( '\0' == *end ){
					while( regs ){
						showReg(regs);
						putReg(regs,value);
						regs = regs->next ;
					}
				} else 
					fprintf( stderr, "Invalid value '%s', use hex\n", argv[1+parse_arguments] );
			}
		} else
			fprintf (stderr, "Nothing matched %s\n", argv[parse_arguments]);
	}
	return 1;
}
