// $Id: script.c 148 2004-09-30 14:05:37Z MouseJstr $
//#define DEBUG_FUNCIN
//#define DEBUG_DISP
//#define DEBUG_RUN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifndef LCCWIN32
#include <sys/time.h>
#endif

#include <time.h>
#include <math.h>

#include "socket.h"
#include "timer.h"
#include "malloc.h"
#include "lock.h"

#include "atcommand.h"
#include "lang.h"
#include "battle.h"
#include "chat.h"
#include "chrif.h"
#include "clif.h"
#include "db.h"
#include "guild.h"
#include "intif.h"
#include "itemdb.h"
#include "lock.h"
#include "map.h"
#include "mob.h"
#include "npc.h"
#include "party.h"
#include "pc.h"
#include "script.h"
#include "skill.h"
#include "storage.h"

#include "../common/showmsg.h"
#include "../common/strlib.h"

#ifdef MEMWATCH
#include "memwatch.h"
#endif

//
// struct script_state* st;
//

/// Returns the script_data at the target index
#define script_getdata(st,i) ( &((st)->stack->stack_data[(st)->start + (i)]) )
/// Returns if the stack contains data at the target index
#define script_hasdata(st,i) ( (st)->end > (st)->start + (i) )
/// Returns the index of the last data in the stack
#define script_lastdata(st) ( (st)->end - (st)->start - 1 )
/// Pushes an int into the stack
#define script_pushint(st,val) push_val((st)->stack, C_INT, (val))
/// Pushes a string into the stack (script engine frees it automatically)
#define script_pushstr(st,val) push_str((st)->stack, C_STR, (val))
/// Pushes a copy of a string into the stack
#define script_pushstrcopy(st,val) push_str((st)->stack, C_STR, aStrdup(val))
/// Pushes a constant string into the stack (must never change or be freed)
#define script_pushconststr(st,val) push_str((st)->stack, C_CONSTSTR, (val))
/// Pushes a nil into the stack
#define script_pushnil(st) push_val((st)->stack, C_NOP, 0)
/// Pushes a copy of the data in the target index
#define script_pushcopy(st,i) push_copy((st)->stack, (st)->start + (i))

#define script_isstring(st,i) data_isstring(script_getdata(st,i))
#define script_isint(st,i) data_isint(script_getdata(st,i))

#define script_getnum(st,val) conv_num(st, script_getdata(st,val))
#define script_getstr(st,val) conv_str(st, script_getdata(st,val))
#define script_getref(st,val) ( script_getdata(st,val)->ref )

// Note: "top" functions/defines use indexes relative to the top of the stack
//       -1 is the index of the data at the top

/// Returns the script_data at the target index relative to the top of the stack
#define script_getdatatop(st,i) ( &((st)->stack->stack_data[(st)->stack->sp + (i)]) )
/// Pushes a copy of the data in the target index relative to the top of the stack
#define script_pushcopytop(st,i) push_copy((st)->stack, (st)->stack->sp + (i))
/// Removes the range of values [start,end[ relative to the top of the stack
#define script_removetop(st,start,end) ( pop_stack((st)->stack, ((st)->stack->sp + (start)), (st)->stack->sp + (end)) )

//
// struct script_data* data;
//

/// Returns if the script data is a string
#define data_isstring(data) ( (data)->type == C_STR || (data)->type == C_CONSTSTR )
/// Returns if the script data is an int
#define data_isint(data) ( (data)->type == C_INT )
/// Returns if the script data is a reference
#define data_isreference(data) ( (data)->type == C_NAME )
/// Returns if the script data is a label
#define data_islabel(data) ( (data)->type == C_POS )
/// Returns if the script data is an internal script function label
#define data_isfunclabel(data) ( (data)->type == C_USERFUNC_POS )

/// Returns if this is a reference to a constant
#define reference_toconstant(data) ( str_data[reference_getid(data)].type == C_INT )
/// Returns if this a reference to a param
#define reference_toparam(data) ( str_data[reference_getid(data)].type == C_PARAM )
/// Returns if this a reference to a variable
//##TODO confirm it's C_NAME [FlavioJS]
#define reference_tovariable(data) ( str_data[reference_getid(data)].type == C_NAME )
/// Returns the unique id of the reference (id and index)
#define reference_getuid(data) ( (data)->u.num )
/// Returns the id of the reference
#define reference_getid(data) ( (int32)(reference_getuid(data) & 0x00ffffff) )
/// Returns the array index of the reference
#define reference_getindex(data) ( (int32)(((uint32)(reference_getuid(data) & 0xff000000)) >> 24) )
/// Returns the name of the reference
#define reference_getname(data) ( str_buf + str_data[reference_getid(data)].str )
/// Returns the linked list of uid-value pairs of the reference (can be NULL)
#define reference_getref(data) ( (data)->ref )
/// Returns the value of the constant
#define reference_getconstant(data) ( str_data[reference_getid(data)].val )
/// Returns the type of param
#define reference_getparamtype(data) ( str_data[reference_getid(data)].val )

/// Composes the uid of a reference from the id and the index
#define reference_uid(id,idx) ( (int32)((((uint32)(id)) & 0x00ffffff) | (((uint32)(idx)) << 24)) )

#define FETCH(n, t) \
    if( script_hasdata(st,n) ) \
        (t)=script_getnum(st,n);


#define SCRIPT_BLOCK_SIZE 256
enum
{ LABEL_NEXTLINE = 1, LABEL_START };
static unsigned char *script_buf;
static int script_pos, script_size;

#define not_server_variable(prefix) ( (prefix) != '$' && (prefix) != '.')
#define not_array_variable(prefix) ( (prefix) != '$' && (prefix) != '@' && (prefix) != '.' )
#define is_string_variable(name) ( (name)[strlen(name) - 1] == '$' )

char *str_buf;
int  str_pos, str_size;
static struct
{
    int  type;
    int  str;
    int  backpatch;
    int  label;
    int  (*func) ();
    int  val;
    int  next;
}   *str_data;
int  str_num = LABEL_START, str_data_size;
int  str_hash[16];

static struct dbt *mapreg_db = NULL;
static struct dbt *mapregstr_db = NULL;
static int mapreg_dirty = -1;
char mapreg_txt[256] = "save/mapreg.txt";
static int index_db = 0;
#define MAPREG_AUTOSAVE_INTERVAL	(10*1000)

static struct dbt *scriptlabel_db = NULL;
static struct dbt *userfunc_db = NULL;

struct dbt *script_get_label_db ()
{
    return scriptlabel_db;
}

struct dbt *script_get_userfunc_db ()
{
    if (!userfunc_db)
        userfunc_db = strdb_init (50);
    return userfunc_db;
}

int scriptlabel_final (void *k __attribute__ ((unused)),
                       void *d __attribute__ ((unused)),
                       va_list ap __attribute__ ((unused)))
{
    return 0;
}

static char pos[11][100] =
    { "0", "1", "2", "3", "4", "5", "6",
    "7", "8", "9", "10"
};

static struct Script_Config
{
    int  warn_func_no_comma;
    int  warn_cmd_no_comma;
    int  warn_func_mismatch_paramnum;
    int  warn_cmd_mismatch_paramnum;
    int  check_cmdcount;
    int  check_gotocount;
    int  db_count;
} script_config;
static int parse_cmd_if = 0;
static int parse_cmd;

/*==========================================
 * ローカルプロトタイプ宣言 (必要な物のみ) 
 *------------------------------------------
 */
unsigned char *parse_subexpr (unsigned char *, int);
int  buildin_mes (struct script_state *st);
int  buildin_mesn (struct script_state *st);
int  buildin_mesq (struct script_state *st);
int  buildin_goto (struct script_state *st);
int  buildin_callsub (struct script_state *st);
int  buildin_callfunc (struct script_state *st);
int  buildin_return (struct script_state *st);
int  buildin_getarg (struct script_state *st);
int  buildin_next (struct script_state *st);
int  buildin_close (struct script_state *st);
int  buildin_close2 (struct script_state *st);
int  buildin_close3 (struct script_state *st);
int  buildin_menu (struct script_state *st);
int  buildin_rand (struct script_state *st);
int  buildin_pow (struct script_state *st);
int  buildin_warp (struct script_state *st);
int  buildin_isat (struct script_state *st);
int  buildin_areawarp (struct script_state *st);
int  buildin_heal (struct script_state *st);
int  buildin_itemheal (struct script_state *st);
int  buildin_percentheal (struct script_state *st);
int  buildin_jobchange (struct script_state *st);
int  buildin_input (struct script_state *st);
int  buildin_setlook (struct script_state *st);
int  buildin_set (struct script_state *st);
int  buildin_setarray (struct script_state *st);
int  buildin_cleararray (struct script_state *st);
int  buildin_copyarray (struct script_state *st);
int  buildin_getarraysize (struct script_state *st);
int  buildin_deletearray (struct script_state *st);
int  buildin_getelementofarray (struct script_state *st);
int  buildin_if (struct script_state *st);
int  buildin_getitem (struct script_state *st);
int  buildin_getitem2 (struct script_state *st);
int  buildin_makeitem (struct script_state *st);
int  buildin_delitem (struct script_state *st);
int  buildin_viewpoint (struct script_state *st);
int  buildin_countitem (struct script_state *st);
int  buildin_checkweight (struct script_state *st);
int  buildin_readparam (struct script_state *st);
int  buildin_getcharid (struct script_state *st);
int  buildin_getpartyname (struct script_state *st);
int  buildin_getpartymember (struct script_state *st);
int  buildin_getguildname (struct script_state *st);
int  buildin_getguildmaster (struct script_state *st);
int  buildin_getguildmasterid (struct script_state *st);
int  buildin_strcharinfo (struct script_state *st);
int  buildin_getequipid (struct script_state *st);
int  buildin_getequipname (struct script_state *st);
int  buildin_getbrokenid (struct script_state *st); // [Valaris]
int  buildin_repair (struct script_state *st);  // [Valaris]
int  buildin_getequipisequiped (struct script_state *st);
int  buildin_getequipisenableref (struct script_state *st);
int  buildin_getequipisidentify (struct script_state *st);
int  buildin_getequiprefinerycnt (struct script_state *st);
int  buildin_getequipweaponlv (struct script_state *st);
int  buildin_getequippercentrefinery (struct script_state *st);
int  buildin_successrefitem (struct script_state *st);
int  buildin_failedrefitem (struct script_state *st);
int  buildin_cutin (struct script_state *st);
int  buildin_cutincard (struct script_state *st);
int  buildin_statusup (struct script_state *st);
int  buildin_statusup2 (struct script_state *st);
int  buildin_bonus (struct script_state *st);
int  buildin_bonus2 (struct script_state *st);
int  buildin_bonus3 (struct script_state *st);
int  buildin_skill (struct script_state *st);
int  buildin_setskill (struct script_state *st);
int  buildin_guildskill (struct script_state *st);
int  buildin_getskilllv (struct script_state *st);
int  buildin_getgdskilllv (struct script_state *st);
int  buildin_basicskillcheck (struct script_state *st);
int  buildin_getgmlevel (struct script_state *st);
int  buildin_end (struct script_state *st);
int  buildin_getopt2 (struct script_state *st);
int  buildin_setopt2 (struct script_state *st);
int  buildin_checkoption (struct script_state *st);
int  buildin_setoption (struct script_state *st);
int  buildin_guildgetexp (struct script_state *st);
int  buildin_guildchangegm (struct script_state *st);
int  buildin_logmes (struct script_state *st);      // this command actls as MES but rints info into LOG file either SQL/TXT [Lupus]
int  buildin_summon (struct script_state *st);      // summons a slave monster [Celest]
int  buildin_setcart (struct script_state *st);
int  buildin_checkcart (struct script_state *st);   // check cart [Valaris]
int  buildin_setfalcon (struct script_state *st);
int  buildin_checkfalcon (struct script_state *st); // check falcon [Valaris]
int  buildin_setriding (struct script_state *st);
int  buildin_checkriding (struct script_state *st); // check for pecopeco [Valaris]
int  buildin_savepoint (struct script_state *st);
int  buildin_gettimetick (struct script_state *st);
int  buildin_gettime (struct script_state *st);
int  buildin_gettimestr (struct script_state *st);
int  buildin_openstorage (struct script_state *st);
int  buildin_guildopenstorage (struct script_state *st);
int  buildin_itemskill (struct script_state *st);
int  buildin_monster (struct script_state *st);
int  buildin_areamonster (struct script_state *st);
int  buildin_killmonster (struct script_state *st);
int  buildin_killmonsterall (struct script_state *st);
int  buildin_doevent (struct script_state *st);
int  buildin_donpcevent (struct script_state *st);
int  buildin_addtimer (struct script_state *st);
int  buildin_deltimer (struct script_state *st);
int  buildin_addtimercount (struct script_state *st);
int  buildin_initnpctimer (struct script_state *st);
int  buildin_stopnpctimer (struct script_state *st);
int  buildin_startnpctimer (struct script_state *st);
int  buildin_setnpctimer (struct script_state *st);
int  buildin_getnpctimer (struct script_state *st);
int  buildin_announce (struct script_state *st);
int  buildin_mapannounce (struct script_state *st);
int  buildin_areaannounce (struct script_state *st);
int  buildin_getusers (struct script_state *st);
int  buildin_getmapusers (struct script_state *st);
int  buildin_getareausers (struct script_state *st);
int  buildin_getareadropitem (struct script_state *st);
int  buildin_enablenpc (struct script_state *st);
int  buildin_disablenpc (struct script_state *st);
int  buildin_enablearena (struct script_state *st); // Added by RoVeRT
int  buildin_disablearena (struct script_state *st);    // Added by RoVeRT
int  buildin_hideoffnpc (struct script_state *st);
int  buildin_hideonnpc (struct script_state *st);
int  buildin_sc_start (struct script_state *st);
int  buildin_sc_startv2 (struct script_state *st);
int  buildin_sc_start2 (struct script_state *st);
int  buildin_sc_end (struct script_state *st);
int  buildin_sc_check (struct script_state *st);    // [Fate]
int  buildin_getscrate (struct script_state *st);
int  buildin_debugmes (struct script_state *st);
int  buildin_resetlvl (struct script_state *st);
int  buildin_resetstatus (struct script_state *st);
int  buildin_resetskill (struct script_state *st);
int  buildin_changebase (struct script_state *st);
int  buildin_changesex (struct script_state *st);
int  buildin_waitingroom (struct script_state *st);
int  buildin_delwaitingroom (struct script_state *st);
int  buildin_enablewaitingroomevent (struct script_state *st);
int  buildin_disablewaitingroomevent (struct script_state *st);
int  buildin_getwaitingroomstate (struct script_state *st);
int  buildin_warpwaitingpc (struct script_state *st);
int  buildin_attachrid (struct script_state *st);
int  buildin_detachrid (struct script_state *st);
int  buildin_isloggedin (struct script_state *st);
int  buildin_setmapflagnosave (struct script_state *st);
int  buildin_setmapflag (struct script_state *st);
int  buildin_removemapflag (struct script_state *st);
int  buildin_pvpon (struct script_state *st);
int  buildin_pvpoff (struct script_state *st);
int  buildin_gvgon (struct script_state *st);
int  buildin_gvgoff (struct script_state *st);
int  buildin_emotion (struct script_state *st);
int  buildin_maprespawnguildid (struct script_state *st);
int  buildin_agitstart (struct script_state *st);   // <Agit>
int  buildin_agitend (struct script_state *st);
int  buildin_agitcheck (struct script_state *st);   // <Agitcheck>
int  buildin_flagemblem (struct script_state *st);  // Flag Emblem
int  buildin_getcastlename (struct script_state *st);
int  buildin_getcastledata (struct script_state *st);
int  buildin_setcastledata (struct script_state *st);
int  buildin_requestguildinfo (struct script_state *st);
int  buildin_getequipcardcnt (struct script_state *st);
int  buildin_successremovecards (struct script_state *st);
int  buildin_failedremovecards (struct script_state *st);
int  buildin_marriage (struct script_state *st);
int  buildin_wedding_effect (struct script_state *st);
int  buildin_divorce (struct script_state *st);
int  buildin_getitemname (struct script_state *st);
int  buildin_getspellinvocation (struct script_state *st);  // [Fate]
int  buildin_getanchorinvocation (struct script_state *st); // [Fate]
int  buildin_getexp (struct script_state *st);
int  buildin_getinventorylist (struct script_state *st);
int  buildin_getskilllist (struct script_state *st);
int  buildin_get_pool_skills (struct script_state *st); // [fate]
int  buildin_get_activated_pool_skills (struct script_state *st);   // [fate]
int  buildin_get_unactivated_pool_skills (struct script_state *st);   // [PO]
int  buildin_activate_pool_skill (struct script_state *st); // [fate]
int  buildin_deactivate_pool_skill (struct script_state *st);   // [fate]
int  buildin_check_pool_skill (struct script_state *st);    // [fate]
//int  buildin_getskilllist (struct script_state *st);
//int  buildin_getskilllist (struct script_state *st);
int  buildin_clearitem (struct script_state *st);
int  buildin_classchange (struct script_state *st);
int  buildin_misceffect (struct script_state *st);
int  buildin_soundeffect (struct script_state *st);
//int  buildin_setcastledata (struct script_state *st);
int  buildin_mapwarp (struct script_state *st);
int  buildin_inittimer (struct script_state *st);
int  buildin_stoptimer (struct script_state *st);
int  buildin_cmdothernpc (struct script_state *st);
int  buildin_mobcount (struct script_state *st);
int  buildin_strmobinfo (struct script_state *st);  // Script for displaying mob info [Valaris]
int  buildin_guardian (struct script_state *st);    // Script for displaying mob info [Valaris]
int  buildin_guardianinfo (struct script_state *st);    // Script for displaying mob info [Valaris]
int  buildin_npcskilleffect (struct script_state *st);  // skill effects for npcs [Valaris]
int  buildin_specialeffect (struct script_state *st);   // special effect script [Valaris]
int  buildin_specialeffect2 (struct script_state *st);  // special effect script [Valaris]
int  buildin_nude (struct script_state *st);    // nude [Valaris]
int  buildin_gmcommand (struct script_state *st);   // [MouseJstr]
int  buildin_movenpc (struct script_state *st); // [MouseJstr]
int  buildin_npcwarp (struct script_state *st); // [remoitnane]
int  buildin_message (struct script_state *st); // [MouseJstr]
int  buildin_npctalk (struct script_state *st); // [Valaris]
int  buildin_npcwalkto (struct script_state *st); // [Valaris]
int  buildin_npcstop (struct script_state *st); // [Valaris]
int  buildin_npcspeed (struct script_state *st); // [Valaris]
int  buildin_hasitems (struct script_state *st);    // [Valaris]
int  buildin_getlook (struct script_state *st); //Lorky [Lupus]
int  buildin_getsavepoint (struct script_state *st);    //Lorky [Lupus]
int  buildin_getmapxy (struct script_state *st);    //Lorky [Lupus]
int  buildin_getpartnerid (struct script_state *st);    // [Fate]
int  buildin_areatimer (struct script_state *st);   // [Jaxad0127]
int  buildin_isin (struct script_state *st);    // [Jaxad0127]
int  buildin_shop (struct script_state *st);    // [MadCamel]
int  buildin_isdead (struct script_state *st);  // [Jaxad0127]
int  buildin_fakenpcname (struct script_state *st); //[Kage]
int  buildin_unequip_by_id (struct script_state *st);   // [Freeyorp]
int  buildin_setcollision (struct script_state *st); // [4144]
int  buildin_spell (struct script_state *st); // [4144]
int  buildin_npcclick (struct script_state *st); // [4144]
int  buildin_npcattach (struct script_state *st); // [4144]
int  buildin_dispbottom (struct script_state *st); //added from jA [Lupus]
int  buildin_recovery (struct script_state *st); // [4144]
int  buildin_getmapmobs (struct script_state *st); //end jA addition
int  buildin_getstrlen (struct script_state *st); //strlen [Valaris]
int  buildin_charisalpha (struct script_state *st); //isalpha [Valaris]
int  buildin_compare (struct script_state *st); // Lordalfa - To bring strstr to scripting Engine.
int  buildin_getiteminfo (struct script_state *st); //[Lupus] returns Items Buy / sell Price, etc info
int  buildin_setiteminfo (struct script_state *st); //[Lupus] set Items Buy / sell Price, etc info
// [zBuffer] List of mathematics commands --->
int  buildin_sqrt (struct script_state *st);
int  buildin_distance (struct script_state *st);
// <--- [zBuffer] List of mathematics commands
// [zBuffer] List of dynamic var commands --->
int  buildin_getd (struct script_state *st);
int  buildin_setd (struct script_state *st);
// <--- [zBuffer] List of dynamic var commands
int  buildin_callshop (struct script_state *st); // [Skotlex]
int  buildin_npcshopitem (struct script_state *st); // [Lance]
int  buildin_npcshopadditem (struct script_state *st);
int  buildin_npcshopdelitem (struct script_state *st);
int  buildin_equip (struct script_state *st);
int  buildin_setitemscript (struct script_state *st); //Set NEW item bonus script. Lupus
int  buildin_getmonsterinfo (struct script_state *st);// Lupus
int  buildin_axtoi (struct script_state *st);
int  buildin_atoi (struct script_state *st);
int  buildin_rid2name (struct script_state *st);
int  buildin_setnpcclass (struct script_state *st); // [4144]
int  buildin_getnpcclass (struct script_state *st); // [4144]
int  buildin_settempnpcclass (struct script_state *st); // [4144]
int  buildin_l (struct script_state *st); // [4144]
int  buildin_setlang (struct script_state *st); // [4144]
int  buildin_getlang (struct script_state *st); // [4144]
int  buildin_geta (struct script_state *st); // [4144]
int  buildin_seta (struct script_state *st); // [4144]
int  buildin_geta2 (struct script_state *st); // [4144]
int  buildin_seta2 (struct script_state *st); // [4144]
int  buildin_geta4 (struct script_state *st); // [4144]
int  buildin_seta4 (struct script_state *st); // [4144]
int  buildin_geta8 (struct script_state *st); // [4144]
int  buildin_seta8 (struct script_state *st); // [4144]
int  buildin_geta16 (struct script_state *st); // [4144]
int  buildin_seta16 (struct script_state *st); // [4144]
int  buildin_getclientversion (struct script_state *st); // [4144]
int  buildin_g (struct script_state *st); // [4144]

void push_val (struct script_stack *stack, int type, int val);
int  run_func (struct script_state *st);

int  mapreg_setreg (int num, int val);
int  mapreg_setregstr (int num, const char *str);

struct
{
    int  (*func) ();
    char *name;
    char *arg;
} buildin_func[] =
{
    {
    buildin_mes, "mes", "s"},
    {
    buildin_mesn, "mesn", "s"},
    {
    buildin_mesq, "mesq", "s"},
    {
    buildin_next, "next", ""},
    {
    buildin_close, "close", ""},
    {
    buildin_close2, "close2", ""},
    {
    buildin_close2, "close3", ""},
    {
    buildin_menu, "menu", "*"},
    {
    buildin_goto, "goto", "l"},
    {
    buildin_callsub, "callsub", "i*"},
    {
    buildin_callfunc, "callfunc", "s*"},
    {
    buildin_return, "return", "*"},
    {
    buildin_getarg, "getarg", "i"},
    {
    buildin_jobchange, "jobchange", "i*"},
    {
    buildin_input, "input", "*"},
    {
    buildin_warp, "warp", "sii"},
    {
    buildin_isat, "isat", "sii"},
    {
    buildin_areawarp, "areawarp", "siiiisii"},
    {
    buildin_setlook, "setlook", "ii"},
    {
    buildin_set, "set", "ii"},
    {
    buildin_setarray, "setarray", "ii*"},
    {
    buildin_cleararray, "cleararray", "iii"},
    {
    buildin_copyarray, "copyarray", "iii"},
    {
    buildin_getarraysize, "getarraysize", "i"},
    {
    buildin_deletearray, "deletearray", "ii"},
    {
    buildin_getelementofarray, "getelementofarray", "ii"},
    {
    buildin_if, "if", "i*"},
    {
    buildin_getitem, "getitem", "ii**"},
    {
    buildin_getitem2, "getitem2", "iiiiiiiii*"},
    {
    buildin_makeitem, "makeitem", "iisii"},
    {
    buildin_delitem, "delitem", "ii"},
    {
    buildin_cutin, "cutin", "si"},
    {
    buildin_cutincard, "cutincard", "i"},
    {
    buildin_viewpoint, "viewpoint", "iiiii"},
    {
    buildin_heal, "heal", "ii"},
    {
    buildin_itemheal, "itemheal", "ii"},
    {
    buildin_percentheal, "percentheal", "ii"},
    {
    buildin_rand, "rand", "i*"},
    {
    buildin_pow, "pow", "ii"},
    {
    buildin_countitem, "countitem", "i"},
    {
    buildin_checkweight, "checkweight", "ii"},
    {
    buildin_readparam, "readparam", "i*"},
    {
    buildin_getcharid, "getcharid", "i*"},
    {
    buildin_getpartyname, "getpartyname", "i"},
    {
    buildin_getpartymember, "getpartymember", "i"},
    {
    buildin_getguildname, "getguildname", "i"},
    {
    buildin_getguildmaster, "getguildmaster", "i"},
    {
    buildin_getguildmasterid, "getguildmasterid", "i"},
    {
    buildin_strcharinfo, "strcharinfo", "i"},
    {
    buildin_getequipid, "getequipid", "i"},
    {
    buildin_getequipname, "getequipname", "i"},
    {
    buildin_getbrokenid, "getbrokenid", "i"},   // [Valaris]
    {
    buildin_repair, "repair", "i"}, // [Valaris]
    {
    buildin_getequipisequiped, "getequipisequiped", "i"},
    {
    buildin_getequipisenableref, "getequipisenableref", "i"},
    {
    buildin_getequipisidentify, "getequipisidentify", "i"},
    {
    buildin_getequiprefinerycnt, "getequiprefinerycnt", "i"},
    {
    buildin_getequipweaponlv, "getequipweaponlv", "i"},
    {
    buildin_getequippercentrefinery, "getequippercentrefinery", "i"},
    {
    buildin_successrefitem, "successrefitem", "i"},
    {
    buildin_failedrefitem, "failedrefitem", "i"},
    {
    buildin_statusup, "statusup", "i"},
    {
    buildin_statusup2, "statusup2", "ii"},
    {
    buildin_bonus, "bonus", "ii"},
    {
    buildin_bonus2, "bonus2", "iii"},
    {
    buildin_bonus3, "bonus3", "iiii"},
    {
    buildin_skill, "skill", "ii*"},
    {
    buildin_setskill, "setskill", "ii"},    // [Fate]
    {
    buildin_guildskill, "guildskill", "ii"},
    {
    buildin_getskilllv, "getskilllv", "i"},
    {
    buildin_getgdskilllv, "getgdskilllv", "ii"},
    {
    buildin_basicskillcheck, "basicskillcheck", "*"},
    {
    buildin_getgmlevel, "getgmlevel", "*"},
    {
    buildin_end, "end", ""},
    {
    buildin_getopt2, "getopt2", "i"},
    {
    buildin_setopt2, "setopt2", "i"},
    {
    buildin_end, "break", ""},
    {
    buildin_checkoption, "checkoption", "i"},
    {
    buildin_setoption, "setoption", "i"},
    {
    buildin_guildgetexp, "guildgetexp", "i"},
    {
    buildin_guildchangegm, "guildchangegm", "is"},
    {
    buildin_logmes, "logmes", "s"},
    {
    buildin_summon, "summon", "si"},
    {
    buildin_setcart, "setcart", ""},
    {
    buildin_checkcart, "checkcart", "*"},   //fixed by Lupus (added '*')
    {
    buildin_setfalcon, "setfalcon", ""},
    {
    buildin_checkfalcon, "checkfalcon", "*"},   //fixed by Lupus (fixed wrong pointer, added '*')
    {
    buildin_setriding, "setriding", ""},
    {
    buildin_checkriding, "checkriding", "*"},   //fixed by Lupus (fixed wrong pointer, added '*')
    {
    buildin_savepoint, "save", "sii"},
    {
    buildin_savepoint, "savepoint", "sii"},
    {
    buildin_gettimetick, "gettimetick", "i"},
    {
    buildin_gettime, "gettime", "i"},
    {
    buildin_gettimestr, "gettimestr", "si"},
    {
    buildin_openstorage, "openstorage", "*"},
    {
    buildin_guildopenstorage, "guildopenstorage", "*"},
    {
    buildin_itemskill, "itemskill", "iis"},
    {
    buildin_monster, "monster", "siisii*"},
    {
    buildin_areamonster, "areamonster", "siiiisii*"},
    {
    buildin_killmonster, "killmonster", "ss"},
    {
    buildin_killmonsterall, "killmonsterall", "s"},
    {
    buildin_doevent, "doevent", "s"},
    {
    buildin_donpcevent, "donpcevent", "s"},
    {
    buildin_addtimer, "addtimer", "is"},
    {
    buildin_deltimer, "deltimer", "s"},
    {
    buildin_addtimercount, "addtimercount", "si"},
    {
    buildin_initnpctimer, "initnpctimer", "*"},
    {
    buildin_stopnpctimer, "stopnpctimer", "*"},
    {
    buildin_startnpctimer, "startnpctimer", "*"},
    {
    buildin_setnpctimer, "setnpctimer", "*"},
    {
    buildin_getnpctimer, "getnpctimer", "i*"},
    {
    buildin_announce, "announce", "si"},
    {
    buildin_mapannounce, "mapannounce", "ssi"},
    {
    buildin_areaannounce, "areaannounce", "siiiisi"},
    {
    buildin_getusers, "getusers", "i"},
    {
    buildin_getmapusers, "getmapusers", "s"},
    {
    buildin_getareausers, "getareausers", "siiii"},
    {
    buildin_getareadropitem, "getareadropitem", "siiiii"},
    {
    buildin_enablenpc, "enablenpc", "s"},
    {
    buildin_disablenpc, "disablenpc", "s"},
    {
    buildin_enablearena, "enablearena", ""},    // Added by RoVeRT
    {
    buildin_disablearena, "disablearena", ""},  // Added by RoVeRT
    {
    buildin_hideoffnpc, "hideoffnpc", "s"},
    {
    buildin_hideonnpc, "hideonnpc", "s"},
    {
    buildin_sc_start, "sc_start", "iii*"},
    {
    buildin_sc_startv2, "sc_startv2", "iiii*"},
    {
    buildin_sc_start2, "sc_start2", "iiii*"},
    {
    buildin_sc_end, "sc_end", "i"},
    {
    buildin_sc_check, "sc_check", "i"},
    {
    buildin_getscrate, "getscrate", "ii*"},
    {
    buildin_debugmes, "debugmes", "s"},
    {
    buildin_resetlvl, "resetlvl", "i"},
    {
    buildin_resetstatus, "resetstatus", ""},
    {
    buildin_resetskill, "resetskill", ""},
    {
    buildin_changebase, "changebase", "i"},
    {
    buildin_changesex, "changesex", ""},
    {
    buildin_waitingroom, "waitingroom", "si*"},
    {
    buildin_warpwaitingpc, "warpwaitingpc", "sii"},
    {
    buildin_delwaitingroom, "delwaitingroom", "*"},
    {
    buildin_enablewaitingroomevent, "enablewaitingroomevent", "*"},
    {
    buildin_disablewaitingroomevent, "disablewaitingroomevent", "*"},
    {
    buildin_getwaitingroomstate, "getwaitingroomstate", "i*"},
    {
    buildin_warpwaitingpc, "warpwaitingpc", "sii*"},
    {
    buildin_attachrid, "attachrid", "i"},
    {
    buildin_detachrid, "detachrid", ""},
    {
    buildin_isloggedin, "isloggedin", "i"},
    {
    buildin_setmapflagnosave, "setmapflagnosave", "ssii"},
    {
    buildin_setmapflag, "setmapflag", "si"},
    {
    buildin_removemapflag, "removemapflag", "si"},
    {
    buildin_pvpon, "pvpon", "s"},
    {
    buildin_pvpoff, "pvpoff", "s"},
    {
    buildin_gvgon, "gvgon", "s"},
    {
    buildin_gvgoff, "gvgoff", "s"},
    {
    buildin_emotion, "emotion", "i"},
    {
    buildin_maprespawnguildid, "maprespawnguildid", "sii"},
    {
    buildin_agitstart, "agitstart", ""},    // <Agit>
    {
    buildin_agitend, "agitend", ""},
    {
    buildin_agitcheck, "agitcheck", "i"},   // <Agitcheck>
    {
    buildin_flagemblem, "flagemblem", "i"}, // Flag Emblem
    {
    buildin_getcastlename, "getcastlename", "s"},
    {
    buildin_getcastledata, "getcastledata", "si*"},
    {
    buildin_setcastledata, "setcastledata", "sii"},
    {
    buildin_requestguildinfo, "requestguildinfo", "i*"},
    {
    buildin_getequipcardcnt, "getequipcardcnt", "i"},
    {
    buildin_successremovecards, "successremovecards", "i"},
    {
    buildin_failedremovecards, "failedremovecards", "ii"},
    {
    buildin_marriage, "marriage", "s"},
    {
    buildin_wedding_effect, "wedding", ""},
    {
    buildin_divorce, "divorce", "i"},
    {
    buildin_getitemname, "getitemname", "*"},
    {
    buildin_getspellinvocation, "getspellinvocation", "s"},
    {
    buildin_getanchorinvocation, "getanchorinvocation", "s"},
    {
    buildin_getpartnerid, "getpartnerid2", "i"},
    {
    buildin_getexp, "getexp", "ii"},
    {
    buildin_getinventorylist, "getinventorylist", ""},
    {
    buildin_getskilllist, "getskilllist", ""},
    {
    buildin_get_pool_skills, "getpoolskilllist", ""},
    {
    buildin_get_activated_pool_skills, "getactivatedpoolskilllist", ""},
    {
    buildin_get_unactivated_pool_skills, "getunactivatedpoolskilllist", ""},
    {
    buildin_activate_pool_skill, "poolskill", "i"},
    {
    buildin_deactivate_pool_skill, "unpoolskill", "i"},
    {
    buildin_check_pool_skill, "checkpoolskill", "i"},
    {
    buildin_clearitem, "clearitem", ""},
    {
    buildin_classchange, "classchange", "ii"},
    {
    buildin_misceffect, "misceffect", "i*"},
    {
    buildin_soundeffect, "soundeffect", "si"},
    {
    buildin_strmobinfo, "strmobinfo", "ii"},    // display mob data [Valaris]
    {
    buildin_guardian, "guardian", "siisii*i"},  // summon guardians
    {
    buildin_guardianinfo, "guardianinfo", "i"}, // display guardian data [Valaris]
    {
    buildin_npcskilleffect, "npcskilleffect", "iiii"},  // npc skill effect [Valaris]
    {
    buildin_specialeffect, "specialeffect", "i"},   // npc skill effect [Valaris]
    {
    buildin_specialeffect2, "specialeffect2", "i"}, // skill effect on players[Valaris]
    {
    buildin_nude, "nude", ""},  // nude command [Valaris]
    {
    buildin_mapwarp, "mapwarp", "ssii"},    // Added by RoVeRT
    {
    buildin_inittimer, "inittimer", ""},
    {
    buildin_stoptimer, "stoptimer", ""},
    {
    buildin_cmdothernpc, "cmdothernpc", "ss"},
    {
    buildin_gmcommand, "gmcommand", "*"},   // [MouseJstr]
//  {buildin_movenpc,"movenpc","siis"}, // [MouseJstr]
    {
    buildin_npcwarp, "npcwarp", "iis"}, // [remoitnane]
    {
    buildin_message, "message", "s*"},  // [MouseJstr]
    {
    buildin_npctalk, "npctalk", "*"},   // [Valaris]
    {
    buildin_npcspeed, "npcspeed", "i"}, // [Valaris]
    {
    buildin_npcwalkto, "npcwalkto", "ii"}, // [Valaris]
    {
    buildin_npcstop, "npcstop", ""}, // [Valaris]
    {
    buildin_hasitems, "hasitems", "*"}, // [Valaris]
    {
    buildin_mobcount, "mobcount", "ss"},
    {
    buildin_getlook, "getlook", "i"},
    {
    buildin_getsavepoint, "getsavepoint", "i"},
    {
    buildin_getmapxy, "getmapxy", "siii*"},
    {
    buildin_areatimer, "areatimer", "siiiiis"},
    {
    buildin_isin, "isin", "siiii"},
    {
    buildin_shop, "shop", "s"},
    {
    buildin_isdead, "isdead", "i"},
    {
    buildin_fakenpcname, "fakenpcname", "ssi"},
    {
    buildin_unequip_by_id, "unequipbyid", "i"}, // [Freeyorp]
    {
    buildin_setcollision, "setcollision", "siiiii"}, // [4144]
    {
    buildin_spell, "spell", "ss"}, // [4144]
    {
    buildin_npcclick, "npcclick", "s"}, // [4144]
    {
    buildin_npcattach, "npcattach", "s"}, // [4144]
    {
    buildin_dispbottom, "dispbottom", "s"}, //added from jA [Lupus]
    {
    buildin_recovery, "recovery", "*"}, // [4144]
    {
    buildin_getmapmobs, "getmapmobs", "s"}, //end jA addition
    {
    buildin_getstrlen, "getstrlen", "s"}, //strlen [Valaris]
    {
    buildin_charisalpha, "charisalpha", "si"}, //isalpha [Valaris]
    {
    buildin_getiteminfo, "getiteminfo", "ii"}, //[Lupus] returns Items Buy / sell Price, etc info
    {
    buildin_setiteminfo, "setiteminfo", "iii"}, //[Lupus] set Items Buy / sell Price, etc info
    {
    // [zBuffer] List of mathematics commands --->
    buildin_sqrt, "sqrt", "i"},
    {
    buildin_distance, "distance", "iiii"},
    {
    // <--- [zBuffer] List of mathematics commands
    // [zBuffer] List of dynamic var commands --->
    buildin_getd, "getd", "*"},
    {
    buildin_setd, "setd", "*"},
    // <--- [zBuffer] List of dynamic var commands
    {
    buildin_compare, "compare", "ss"}, // Lordalfa - To bring strstr to scripting Engine.
    {
    buildin_callshop, "callshop", "si"}, // [Skotlex]
    {
    buildin_npcshopitem, "npcshopitem", "sii*"}, // [Lance]
    {
    buildin_npcshopadditem, "npcshopadditem", "sii*"},
    {
    buildin_npcshopdelitem, "npcshopdelitem", "si*"},
    {
    buildin_equip, "equip", "i"},
    {
    buildin_setitemscript, "setitemscript", "is*"}, //Set NEW item bonus script. Lupus
    {
    buildin_getmonsterinfo, "getmonsterinfo", "ii"}, //Lupus
    {
    buildin_axtoi, "axtoi", "s"},
    {
    buildin_axtoi, "atoi", "s"},
    {
    buildin_rid2name, "rid2name", "i"},
    {
    buildin_setnpcclass, "setnpcclass", "*"}, // [4144]
    {
    buildin_getnpcclass, "getnpcclass", "*"}, // [4144]
    {
    buildin_settempnpcclass, "settempnpcclass", "*"}, // [4144]
    {
    buildin_l, "l", "s"}, // [4144]
    {
    buildin_getlang, "getlang", "*"}, // [4144]
    {
    buildin_setlang, "setlang", "i*"}, // [4144]
        // End Additions
    {
    buildin_geta, "geta", "si"}, // [4144]
    {
    buildin_seta, "seta", "sii"}, // [4144]
    {
    buildin_geta2, "geta2", "si"}, // [4144]
    {
    buildin_seta2, "seta2", "sii"}, // [4144]
    {
    buildin_geta4, "geta4", "si"}, // [4144]
    {
    buildin_seta4, "seta4", "sii"}, // [4144]
    {
    buildin_geta8, "geta8", "si"}, // [4144]
    {
    buildin_seta8, "seta8", "sii"}, // [4144]
    {
    buildin_geta16, "geta16", "si"}, // [4144]
    {
    buildin_seta16, "seta16", "sii"}, // [4144]
    {
    buildin_getclientversion, "getclientversion", "*"}, // [4144]
    {
    buildin_g, "g", "ss"}, // [4144]
    {
NULL, NULL, NULL},};

//int  buildin_message (struct script_state *st); // [MouseJstr]

enum
{
    C_NOP, C_POS, C_INT, C_PARAM, C_FUNC, C_STR, C_CONSTSTR, C_ARG,
    C_NAME, C_EOL, C_RETINFO,

    C_LOR, C_LAND, C_LE, C_LT, C_GE, C_GT, C_EQ, C_NE,  //operator
    C_XOR, C_OR, C_AND, C_ADD, C_SUB, C_MUL, C_DIV, C_MOD, C_NEG, C_LNOT,
    C_NOT, C_R_SHIFT, C_L_SHIFT
};

/*
/// Reports on the console the src of a script error.
static void script_reportsrc(struct script_state *st)
{
    struct block_list* bl;

    if (st->oid == 0)
        return; //Can't report source.

    bl = map_id2bl(st->oid);
    if (bl == NULL)
        return;

    switch (bl->type)
    {
    case BL_NPC:
        if (bl->m >= 0)
            ShowDebug("Source (NPC): %s at %s (%d,%d)\n", ((struct npc_data *)bl)->name, map[bl->m].name, bl->x, bl->y);
        else
            ShowDebug("Source (NPC): %s (invisible/not on a map)\n", ((struct npc_data *)bl)->name);
        break;
    default:
        if (bl->m >= 0)
            ShowDebug("Source (Non-NPC type %d): name %s at %s (%d,%d)\n", bl->type, status_get_name(bl), map[bl->m].name, bl-
        else
            ShowDebug("Source (Non-NPC type %d): name %s (invisible/not on a map)\n", bl->type, status_get_name(bl));
        break;
    }
}
*/

/// Checks event parameter validity
static void check_event(struct script_state *st __attribute__ ((unused)), const char *evt)
{
    if (evt != NULL && *evt != '\0' && !stristr(evt,"::On"))
    {
        ShowError("NPC event parameter deprecated! Please use 'NPCNAME::OnEVENT' instead of '%s'.\n", evt);
//        script_reportsrc(st);
    }
}

/*==========================================
 * 文字列のハッシュを計算 
 *------------------------------------------
 */
static int calc_hash (const unsigned char *p)
{
    if (!p)
        return 0;

    int  h = 0;
    while (*p)
    {
        h = (h << 1) + (h >> 3) + (h >> 5) + (h >> 8);
        h += *p++;
    }
    return h & 15;
}

/*==========================================
 * str_dataの中に名前があるか検索する 
 *------------------------------------------
 */
// 既存のであれば番号、無ければ-1 
static int search_str (const unsigned char *p)
{
    if (!p)
        return -1;

    int  i;
    i = str_hash[calc_hash (p)];
    while (i)
    {
        if (strcmp (str_buf + str_data[i].str, p) == 0)
        {
            return i;
        }
        i = str_data[i].next;
    }
    return -1;
}

/*==========================================
 * str_dataに名前を登録 
 *------------------------------------------
 */
// 既存のであれば番号、無ければ登録して新規番号 
static int add_str (const unsigned char *p)
{
    if (!p)
        return 0;

    int  i;
    char *lowcase;

    lowcase = strdup (p);
    for (i = 0; lowcase[i]; i++)
        lowcase[i] = tolower (lowcase[i]);
    if ((i = search_str (lowcase)) >= 0)
    {
        free (lowcase);
        return i;
    }
    free (lowcase);
    lowcase = 0;

    i = calc_hash (p);
    if (str_hash[i] == 0)
    {
        str_hash[i] = str_num;
    }
    else
    {
        i = str_hash[i];
        for (;;)
        {
            if (strcmp (str_buf + str_data[i].str, p) == 0)
            {
                return i;
            }
            if (str_data[i].next == 0)
                break;
            i = str_data[i].next;
        }
        str_data[i].next = str_num;
    }
    if (str_num >= str_data_size)
    {
        str_data_size += 128;
        str_data = aRealloc (str_data, sizeof (str_data[0]) * str_data_size);
        memset (str_data + (str_data_size - 128), '\0', 128);
    }
    while (str_pos + strlen (p) + 1 >= str_size)
    {
        str_size += 256;
        str_buf = (char *) aRealloc (str_buf, str_size);
        memset (str_buf + (str_size - 256), '\0', 256);
    }
    strcpy (str_buf + str_pos, p);
    str_data[str_num].type = C_NOP;
    str_data[str_num].str = str_pos;
    str_data[str_num].next = 0;
    str_data[str_num].func = NULL;
    str_data[str_num].backpatch = -1;
    str_data[str_num].label = -1;
    str_pos += strlen (p) + 1;
    return str_num++;
}

/*==========================================
 * スクリプトバッファサイズの確認と拡張 
 *------------------------------------------
 */
static void check_script_buf (int size)
{
    if (script_pos + size >= script_size)
    {
        script_size += SCRIPT_BLOCK_SIZE;
        script_buf = (char *) aRealloc (script_buf, script_size);
        memset (script_buf + script_size - SCRIPT_BLOCK_SIZE, '\0',
                SCRIPT_BLOCK_SIZE);
    }
}

/*==========================================
 * スクリプトバッファに１バイト書き込む 
 *------------------------------------------
 */
static void add_scriptb (int a)
{
    check_script_buf (1);
    script_buf[script_pos++] = a;
}

/*==========================================
 * スクリプトバッファにデータタイプを書き込む 
 *------------------------------------------
 */
static void add_scriptc (int a)
{
    while (a >= 0x40)
    {
        add_scriptb ((a & 0x3f) | 0x40);
        a = (a - 0x40) >> 6;
    }
    add_scriptb (a & 0x3f);
}

/*==========================================
 * スクリプトバッファに整数を書き込む 
 *------------------------------------------
 */
static void add_scripti (int a)
{
    while (a >= 0x40)
    {
        add_scriptb (a | 0xc0);
        a = (a - 0x40) >> 6;
    }
    add_scriptb (a | 0x80);
}

/*==========================================
 * スクリプトバッファにラベル/変数/関数を書き込む 
 *------------------------------------------
 */
// 最大16Mまで 
static void add_scriptl (int l)
{
    int  backpatch = str_data[l].backpatch;

    switch (str_data[l].type)
    {
        case C_POS:
            add_scriptc (C_POS);
            add_scriptb (str_data[l].label);
            add_scriptb (str_data[l].label >> 8);
            add_scriptb (str_data[l].label >> 16);
            break;
        case C_NOP:
               // ラベルの可能性があるのでbackpatch用データ埋め込み 
            add_scriptc (C_NAME);
            str_data[l].backpatch = script_pos;
            add_scriptb (backpatch);
            add_scriptb (backpatch >> 8);
            add_scriptb (backpatch >> 16);
            break;
        case C_INT:
            add_scripti (str_data[l].val);
            break;
        default:
               // もう他の用途と確定してるので数字をそのまま 
            add_scriptc (C_NAME);
            add_scriptb (l);
            add_scriptb (l >> 8);
            add_scriptb (l >> 16);
            break;
    }
}

/*==========================================
 * ラベルを解決する 
 *------------------------------------------
 */
void set_label (int l, int pos)
{
    int  i, next;

    str_data[l].type = C_POS;
    str_data[l].label = pos;
    for (i = str_data[l].backpatch; i >= 0 && i != 0x00ffffff;)
    {
        next = (*(int *) (script_buf + i)) & 0x00ffffff;
        script_buf[i - 1] = C_POS;
        script_buf[i] = pos;
        script_buf[i + 1] = pos >> 8;
        script_buf[i + 2] = pos >> 16;
        i = next;
    }
}

/*==========================================
 * スペース/コメント読み飛ばし 
 *------------------------------------------
 */
static unsigned char *skip_space (unsigned char *p)
{
    if (!p)
        return 0;

    while (1)
    {
        while (isspace (*p))
            p++;
        if (p[0] == '/' && p[1] == '/')
        {
            while (*p && *p != '\n')
                p++;
        }
        else if (p[0] == '/' && p[1] == '*')
        {
            p++;
            while (*p && (p[-1] != '*' || p[0] != '/'))
                p++;
            if (*p)
                p++;
        }
        else
            break;
    }
    return p;
}

/*==========================================
 * １単語スキップ 
 *------------------------------------------
 */
static unsigned char *skip_word (unsigned char *p)
{
    if (!p)
        return 0;

    // prefix
    if (*p == '$')
        p++;                    // MAP鯖内共有変数用 
    if (*p == '@')
        p++;                    // 一時的変数用(like weiss) 
    if (*p == '#')
        p++;                    // account変数用 
    if (*p == '#')
        p++;                    // ワールドaccount変数用 
    if (*p == 'l')
        p++;                    // 一時的変数用(like weiss) 

    while (isalnum (*p) || *p == '_' || *p >= 0x81)
        if (*p >= 0x81 && p[1])
        {
            p += 2;
        }
        else
            p++;

    // postfix
    if (*p == '$')
        p++;                    // 文字列変数 

    return p;
}

static unsigned char *startptr;
static int startline;

/*==========================================
 * エラーメッセージ出力 
 *------------------------------------------
 */
static void disp_error_message (const char *mes, const unsigned char *pos)
{
    if (!mes)
        return;

    int  line, c = 0, i;
    unsigned char *p, *linestart, *lineend;

    for (line = startline, p = startptr; p && *p; line++)
    {
        linestart = p;
        lineend = strchr (p, '\n');
        if (lineend)
        {
            c = *lineend;
            *lineend = 0;
        }
        if (lineend == NULL || pos < lineend)
        {
            printf ("%s line %d : ", mes, line);
            for (i = 0;
                 (linestart[i] != '\r') && (linestart[i] != '\n')
                 && linestart[i]; i++)
            {
                if (linestart + i != pos)
                    printf ("%c", linestart[i]);
                else
                    printf ("\'%c\'", linestart[i]);
            }
            printf ("\a\n");
            if (lineend)
                *lineend = c;
            return;
        }
        *lineend = c;
        p = lineend + 1;
    }
}

/*==========================================
 * 項の解析 
 *------------------------------------------
 */
unsigned char *parse_simpleexpr (unsigned char *p)
{
    if (!p)
        return 0;

    p = skip_space (p);

#ifdef DEBUG_FUNCIN
    if (battle_config.etc_log)
        printf ("parse_simpleexpr %s\n", p);
#endif
    if (*p == ';' || *p == ',')
    {
        disp_error_message ("unexpected expr end", p);
        exit (1);
    }
    if (*p == '(')
    {

        p = parse_subexpr (p + 1, -1);
        p = skip_space (p);
        if ((*p++) != ')')
        {
            disp_error_message ("unmatch ')'", p);
            exit (1);
        }
    }
    else if (isdigit (*p) || ((*p == '-' || *p == '+') && isdigit (p[1])))
    {
        char *np;
        int  i;
        i = strtoul (p, &np, 0);
        add_scripti (i);
        p = np;
    }
    else if (*p == '"')
    {
        add_scriptc (C_STR);
        p++;
        while (*p && *p != '"')
        {
            if (p[-1] <= 0x7e && *p == '\\')
                p++;
            else if (*p == '\n')
            {
                disp_error_message ("unexpected newline @ string", p);
                exit (1);
            }
            add_scriptb (*p++);
        }
        if (!*p)
        {
            disp_error_message ("unexpected eof @ string", p);
            exit (1);
        }
        add_scriptb (0);
        p++;                    //'"'
    }
    else
    {
        int  c, l;
        char *p2;
        // label , register , function etc
        if (skip_word (p) == p)
        {
            disp_error_message ("unexpected character", p);
            exit (1);
        }
        p2 = skip_word (p);
        c = *p2;
        *p2 = 0;                //名前をadd_strする 
        l = add_str (p);

        parse_cmd = l;          // warn_*_mismatch_paramnumのために必要 
        if (l == search_str ("if")) // warn_cmd_no_commaのために必要 
            parse_cmd_if++;
/*
		// 廃止予定のl14/l15,およびプレフィックスｌの警告 
		if(	strcmp(str_buf+str_data[l].str,"l14")==0 ||
			strcmp(str_buf+str_data[l].str,"l15")==0 ){
			disp_error_message("l14 and l15 is DEPRECATED. use @menu instead of l15.",p);
		}else if(str_buf[str_data[l].str]=='l'){
			disp_error_message("prefix 'l' is DEPRECATED. use prefix '@' instead.",p2);
		}
*/
        *p2 = c;
        p = p2;

        if (str_data[l].type != C_FUNC && c == '[')
        {
            // array(name[i] => getelementofarray(name,i) )
            add_scriptl (search_str ("getelementofarray"));
            add_scriptc (C_ARG);
            add_scriptl (l);
            p = parse_subexpr (p + 1, -1);
            p = skip_space (p);
            if ((*p++) != ']')
            {
                disp_error_message ("unmatch ']'", p);
                exit (1);
            }
            add_scriptc (C_FUNC);
        }
        else
            add_scriptl (l);

    }

#ifdef DEBUG_FUNCIN
    if (battle_config.etc_log)
        printf ("parse_simpleexpr end %s\n", p);
#endif
    return p;
}

/*==========================================
 * 式の解析 
 *------------------------------------------
 */
unsigned char *parse_subexpr (unsigned char *p, int limit)
{
    int  op, opl, len;
    char *tmpp;

    if (!p)
        return 0;

#ifdef DEBUG_FUNCIN
    if (battle_config.etc_log)
        printf ("parse_subexpr %s\n", p);
#endif
    p = skip_space (p);

    if (*p == '-')
    {
        tmpp = skip_space (p + 1);
        if (*tmpp == ';' || *tmpp == ',')
        {
            add_scriptl (LABEL_NEXTLINE);
            p++;
            return p;
        }
    }
    tmpp = p;
    if ((op = C_NEG, *p == '-') || (op = C_LNOT, *p == '!')
        || (op = C_NOT, *p == '~'))
    {
        p = parse_subexpr (p + 1, 100);
        add_scriptc (op);
    }
    else
        p = parse_simpleexpr (p);
    p = skip_space (p);
    while (((op = C_ADD, opl = 6, len = 1, *p == '+') ||
            (op = C_SUB, opl = 6, len = 1, *p == '-') ||
            (op = C_MUL, opl = 7, len = 1, *p == '*') ||
            (op = C_DIV, opl = 7, len = 1, *p == '/') ||
            (op = C_MOD, opl = 7, len = 1, *p == '%') ||
            (op = C_FUNC, opl = 8, len = 1, *p == '(') ||
            (op = C_LAND, opl = 1, len = 2, *p == '&' && p[1] == '&') ||
            (op = C_AND, opl = 5, len = 1, *p == '&') ||
            (op = C_LOR, opl = 0, len = 2, *p == '|' && p[1] == '|') ||
            (op = C_OR, opl = 4, len = 1, *p == '|') ||
            (op = C_XOR, opl = 3, len = 1, *p == '^') ||
            (op = C_EQ, opl = 2, len = 2, *p == '=' && p[1] == '=') ||
            (op = C_NE, opl = 2, len = 2, *p == '!' && p[1] == '=') ||
            (op = C_R_SHIFT, opl = 5, len = 2, *p == '>' && p[1] == '>') ||
            (op = C_GE, opl = 2, len = 2, *p == '>' && p[1] == '=') ||
            (op = C_GT, opl = 2, len = 1, *p == '>') ||
            (op = C_L_SHIFT, opl = 5, len = 2, *p == '<' && p[1] == '<') ||
            (op = C_LE, opl = 2, len = 2, *p == '<' && p[1] == '=') ||
            (op = C_LT, opl = 2, len = 1, *p == '<')) && opl > limit)
    {
        p += len;
        if (op == C_FUNC)
        {
            int  i = 0, func = parse_cmd;
            const char *plist[128];

            if (str_data[func].type != C_FUNC)
            {
                disp_error_message ("expect function", tmpp);
                exit (0);
            }

            add_scriptc (C_ARG);
            do
            {
                plist[i] = p;
                p = parse_subexpr (p, -1);
                p = skip_space (p);
                if (*p == ',')
                    p++;
                else if (*p != ')' && script_config.warn_func_no_comma)
                {
                    disp_error_message ("expect ',' or ')' at func params",
                                        p);
                }
                p = skip_space (p);
                i++;
            }
            while (*p && *p != ')' && i < 128);
            plist[i] = p;
            if (*(p++) != ')')
            {
                disp_error_message ("func request '(' ')'", p);
                exit (1);
            }

            if (str_data[func].type == C_FUNC
                && script_config.warn_func_mismatch_paramnum)
            {
                const char *arg = buildin_func[str_data[func].val].arg;
                int  j = 0;
                for (j = 0; arg[j]; j++)
                    if (arg[j] == '*')
                        break;
                if ((arg[j] == 0 && i != j) || (arg[j] == '*' && i < j))
                {
                    disp_error_message ("illegal number of parameters",
                                        plist[(i < j) ? i : j]);
                }
            }
        }
        else
        {
            p = parse_subexpr (p, opl);
        }
        add_scriptc (op);
        p = skip_space (p);
    }
#ifdef DEBUG_FUNCIN
    if (battle_config.etc_log)
        printf ("parse_subexpr end %s\n", p);
#endif
    return p;                   /* return first untreated operator */
}

/*==========================================
 * 式の評価 
 *------------------------------------------
 */
unsigned char *parse_expr (unsigned char *p)
{
    if (!p)
        return 0;

#ifdef DEBUG_FUNCIN
    if (battle_config.etc_log)
        printf ("parse_expr %s\n", p);
#endif
    switch (*p)
    {
        case ')':
        case ';':
        case ':':
        case '[':
        case ']':
        case '}':
            disp_error_message ("unexpected char", p);
            exit (1);
        default:
            break;
    }
    p = parse_subexpr (p, -1);
#ifdef DEBUG_FUNCIN
    if (battle_config.etc_log)
        printf ("parse_expr end %s\n", p);
#endif
    return p;
}

/*==========================================
 * 行の解析 
 *------------------------------------------
 */
unsigned char *parse_line (unsigned char *p)
{
    if (!p)
        return 0;

    int  i = 0, cmd;
    const char *plist[128];
    char *p2;

    p = skip_space (p);
    if (*p == ';')
        return p;

    parse_cmd_if = 0;           // warn_cmd_no_commaのために必要 

    // 最初は関数名 
    p2 = p;
    p = parse_simpleexpr (p);
    p = skip_space (p);

    cmd = parse_cmd;
    if (str_data[cmd].type != C_FUNC)
    {
        disp_error_message ("expect command", p2);
//      exit(0);
    }

    add_scriptc (C_ARG);
    while (p && *p && *p != ';' && i < 128)
    {
        plist[i] = p;

        p = parse_expr (p);
        p = skip_space (p);
        // 引数区切りの,処理 
        if (*p == ',')
            p++;
        else if (*p != ';' && script_config.warn_cmd_no_comma
                 && parse_cmd_if * 2 <= i)
        {
            disp_error_message ("expect ',' or ';' at cmd params", p);
        }
        p = skip_space (p);
        i++;
    }
    plist[i] = p;
    if (!p || *(p++) != ';')
    {
        disp_error_message ("need ';'", p);
        exit (1);
    }
    add_scriptc (C_FUNC);

    if (str_data[cmd].type == C_FUNC
        && script_config.warn_cmd_mismatch_paramnum)
    {
        const char *arg = buildin_func[str_data[cmd].val].arg;
        int  j = 0;
        for (j = 0; arg[j]; j++)
            if (arg[j] == '*')
                break;
        if ((arg[j] == 0 && i != j) || (arg[j] == '*' && i < j))
        {
            disp_error_message ("illegal number of parameters",
                                plist[(i < j) ? i : j]);
        }
    }

    return p;
}

/*==========================================
 * 組み込み関数の追加 
 *------------------------------------------
 */
static void add_buildin_func (void)
{
    int  i, n;
    for (i = 0; buildin_func[i].func; i++)
    {
        n = add_str (buildin_func[i].name);
        str_data[n].type = C_FUNC;
        str_data[n].val = i;
        str_data[n].func = buildin_func[i].func;
    }
}

/*==========================================
 * 定数データベースの読み込み 
 *------------------------------------------
 */
static void read_constdb (void)
{
    FILE *fp;
    char line[2026], name[2026];
    int  val, n, i, type;

    fp = fopen_ ("db/const.txt", "r");
    if (fp == NULL)
    {
        printf ("can't read db/const.txt\n");
        return;
    }
    while (fgets (line, 2020, fp))
    {
        if (line[0] == '/' && line[1] == '/')
            continue;
        type = 0;
        if (sscanf (line, "%1024[A-Za-z0-9_],%d,%d", name, &val, &type) >= 2 ||
            sscanf (line, "%1024[A-Za-z0-9_] %d %d", name, &val, &type) >= 2)
        {
            for (i = 0; name[i]; i++)
                name[i] = tolower (name[i]);
            n = add_str (name);
            if (type == 0)
                str_data[n].type = C_INT;
            else
                str_data[n].type = C_PARAM;
            str_data[n].val = val;
        }
    }
    fclose_ (fp);
}

/*==========================================
 * スクリプトの解析 
 *------------------------------------------
 */
unsigned char *parse_script (unsigned char *src, int line)
{
    unsigned char *p, *tmpp;
    int  i;
    static int first = 1;

    if (!src)
        return 0;

    if (first)
    {
        add_buildin_func ();
        read_constdb ();
    }
    first = 0;
    script_buf =
        (unsigned char *) aCalloc (SCRIPT_BLOCK_SIZE, sizeof (unsigned char));
    script_pos = 0;
    script_size = SCRIPT_BLOCK_SIZE;
    str_data[LABEL_NEXTLINE].type = C_NOP;
    str_data[LABEL_NEXTLINE].backpatch = -1;
    str_data[LABEL_NEXTLINE].label = -1;
    for (i = LABEL_START; i < str_num; i++)
    {
        if (str_data[i].type == C_POS || str_data[i].type == C_NAME)
        {
            str_data[i].type = C_NOP;
            str_data[i].backpatch = -1;
            str_data[i].label = -1;
        }
    }

    // 外部用label dbの初期化 
    if (scriptlabel_db != NULL)
        strdb_final (scriptlabel_db, scriptlabel_final);
    scriptlabel_db = strdb_init (50);

    // for error message
    startptr = src;
    startline = line;

    p = src;
    p = skip_space (p);
    if (*p != '{')
    {
        disp_error_message ("not found '{'", p);
        return NULL;
    }
    for (p++; p && *p && *p != '}';)
    {
        p = skip_space (p);
        // labelだけ特殊処理 
        tmpp = skip_space (skip_word (p));
        if (*tmpp == ':')
        {
            int  l, c;

            c = *skip_word (p);
            *skip_word (p) = 0;
            l = add_str (p);
            if (str_data[l].label != -1)
            {
                *skip_word (p) = c;
                disp_error_message ("dup label ", p);
                exit (1);
            }
            set_label (l, script_pos);
            strdb_insert (scriptlabel_db, p, script_pos);   // 外部用label db登録 
            *skip_word (p) = c;
            p = tmpp + 1;
            continue;
        }

        // 他は全部一緒くた 
        p = parse_line (p);
        p = skip_space (p);
        add_scriptc (C_EOL);

        set_label (LABEL_NEXTLINE, script_pos);
        str_data[LABEL_NEXTLINE].type = C_NOP;
        str_data[LABEL_NEXTLINE].backpatch = -1;
        str_data[LABEL_NEXTLINE].label = -1;
    }

    add_scriptc (C_NOP);

    script_size = script_pos;
    script_buf = (char *) aRealloc (script_buf, script_pos + 1);

    // 未解決のラベルを解決 
    for (i = LABEL_START; i < str_num; i++)
    {
        if (str_data[i].type == C_NOP)
        {
            int  j, next;
            str_data[i].type = C_NAME;
            str_data[i].label = i;
            for (j = str_data[i].backpatch; j >= 0 && j != 0x00ffffff;)
            {
                next = (*(int *) (script_buf + j)) & 0x00ffffff;
                script_buf[j] = i;
                script_buf[j + 1] = i >> 8;
                script_buf[j + 2] = i >> 16;
                j = next;
            }
        }
    }

#ifdef DEBUG_DISP
    for (i = 0; i < script_pos; i++)
    {
        if ((i & 15) == 0)
            printf ("%04x : ", i);
        printf ("%02x ", script_buf[i]);
        if ((i & 15) == 15)
            printf ("\n");
    }
    printf ("\n");
#endif

    return script_buf;
}

//
// 実行系 
//
enum
{ STOP = 1, END, RERUNLINE, GOTO, RETFUNC };

/*==========================================
 * ridからsdへの解決 
 *------------------------------------------
 */
struct map_session_data *script_rid2sd (struct script_state *st)
{
    if (!st)
        return 0;

    struct map_session_data *sd = map_id2sd (st->rid);
    if (!sd)
    {
        printf ("script_rid2sd: fatal error ! player not attached!\n");
    }
    return sd;
}

/*==========================================
 * 変数の読み取り 
 *------------------------------------------
 */
int get_val (struct script_state *st, struct script_data *data)
{
    if (!st || !data)
        return 0;

    struct map_session_data *sd = NULL;
    if (data->type == C_NAME)
    {
        char *name = str_buf + str_data[data->u.num & 0x00ffffff].str;
        int idx = strlen (name) - 1;
        if (idx < 0)
            return 0;

        char prefix = *name;
        char postfix = name[idx];

        if (prefix != '$')
        {
            if ((sd = script_rid2sd (st)) == NULL)
                printf ("get_val error name?:%s\n", name);
        }
        if (postfix == '$')
        {

            data->type = C_CONSTSTR;
            if (prefix == '@' || prefix == 'l')
            {
                if (sd)
                    data->u.str = pc_readregstr (sd, data->u.num);
            }
            else if (prefix == '$')
            {
                data->u.str =
                    (char *) numdb_search (mapregstr_db, data->u.num);
            }
            else
            {
                printf ("script: get_val: illegal scope string variable.\n");
                data->u.str = "!!ERROR!!";
            }
            if (data->u.str == NULL)
                data->u.str = "";

        }
        else
        {

            data->type = C_INT;
            if (str_data[data->u.num & 0x00ffffff].type == C_INT)
            {
                data->u.num = str_data[data->u.num & 0x00ffffff].val;
            }
            else if (str_data[data->u.num & 0x00ffffff].type == C_PARAM)
            {
                if (sd)
                    data->u.num =
                        pc_readparam (sd,
                                      str_data[data->u.num & 0x00ffffff].val);
            }
            else if (prefix == '@' || prefix == 'l')
            {
                if (sd)
                    data->u.num = pc_readreg (sd, data->u.num);
            }
            else if (prefix == '$')
            {
                data->u.num = (int) numdb_search (mapreg_db, data->u.num);
            }
            else if (prefix == '#')
            {
                if (name[1] == '#')
                {
                    if (sd)
                        data->u.num = pc_readaccountreg2 (sd, name);
                }
                else
                {
                    if (sd)
                        data->u.num = pc_readaccountreg (sd, name);
                }
            }
            else
            {
                if (sd)
                    data->u.num = pc_readglobalreg (sd, name);
            }
        }
    }
    return 0;
}

/*==========================================
 * 変数の読み取り2 
 *------------------------------------------
 */
void *get_val2 (struct script_state *st, int num)
{
    struct script_data dat;
    dat.type = C_NAME;
    dat.u.num = num;
    get_val (st, &dat);
    if (dat.type == C_INT)
        return (void *) dat.u.num;
    else
        return (void *) dat.u.str;
}

/*==========================================
 * 変数設定用 
 *------------------------------------------
 */
static int set_reg (struct map_session_data *sd, int num, char *name, void *v)
{
    char prefix = *name;
    char postfix = name[strlen (name) - 1];

    if (postfix == '$')
    {
        char *str = (char *) v;
        if (prefix == '@' || prefix == 'l')
        {
            pc_setregstr (sd, num, str);
        }
        else if (prefix == '$')
        {
            mapreg_setregstr (num, str);
        }
        else
        {
            printf ("script: set_reg: illegal scope string variable !");
        }
    }
    else
    {
        // 数値 
        int  val = (int) v;
        if (str_data[num & 0x00ffffff].type == C_PARAM)
        {
            pc_setparam (sd, str_data[num & 0x00ffffff].val, val);
        }
        else if (prefix == '@' || prefix == 'l')
        {
            pc_setreg (sd, num, val);
        }
        else if (prefix == '$')
        {
            mapreg_setreg (num, val);
        }
        else if (prefix == '#')
        {
            if (name[1] == '#')
                pc_setaccountreg2 (sd, name, val);
            else
                pc_setaccountreg (sd, name, val);
        }
        else
        {
            pc_setglobalreg (sd, name, val);
        }
    }
    return 0;
}

/*==========================================
 * 文字列への変換 
 *------------------------------------------
 */
char *conv_str (struct script_state *st, struct script_data *data)
{
    get_val (st, data);
    if (data->type == C_INT)
    {
        char *buf;
        buf = (char *) aCalloc (16, sizeof (char));
        sprintf (buf, "%d", data->u.num);
        data->type = C_STR;
        data->u.str = buf;
#if 1
    }
    else if (data->type == C_NAME)
    {
          // テンポラリ。本来無いはず 
        data->type = C_CONSTSTR;
        data->u.str = str_buf + str_data[data->u.num].str;
#endif
    }
    return data->u.str;
}

/*==========================================
 * 数値へ変換 
 *------------------------------------------
 */
int conv_num (struct script_state *st, struct script_data *data)
{
    char *p;
    get_val (st, data);
    if (data->type == C_STR || data->type == C_CONSTSTR)
    {
        p = data->u.str;
        data->u.num = atoi (p);
        if (data->type == C_STR)
            free (p);
        data->type = C_INT;
    }
    return data->u.num;
}

/*==========================================
 * スタックへ数値をプッシュ 
 *------------------------------------------
 */
void push_val (struct script_stack *stack, int type, int val)
{
    if (stack->sp >= stack->sp_max)
    {
        stack->sp_max += 64;
        stack->stack_data =
            (struct script_data *) aRealloc (stack->stack_data,
                                             sizeof (stack->stack_data[0]) *
                                             stack->sp_max);
        memset (stack->stack_data + (stack->sp_max - 64), 0,
                64 * sizeof (*(stack->stack_data)));
    }
//  if(battle_config.etc_log)
//      printf("push (%d,%d)-> %d\n",type,val,stack->sp);
    stack->stack_data[stack->sp].type = type;
    stack->stack_data[stack->sp].u.num = val;
    stack->sp++;
}

/*==========================================
 * スタックへ文字列をプッシュ 
 *------------------------------------------
 */
void push_str (struct script_stack *stack, int type, unsigned char *str)
{
    if (stack->sp >= stack->sp_max)
    {
        stack->sp_max += 64;
        stack->stack_data =
            (struct script_data *) aRealloc (stack->stack_data,
                                             sizeof (stack->stack_data[0]) *
                                             stack->sp_max);
        memset (stack->stack_data + (stack->sp_max - 64), '\0',
                64 * sizeof (*(stack->stack_data)));
    }
//  if(battle_config.etc_log)
//      printf("push (%d,%x)-> %d\n",type,str,stack->sp);
    stack->stack_data[stack->sp].type = type;
    stack->stack_data[stack->sp].u.str = str;
    stack->sp++;
}

/*==========================================
 * スタックへ複製をプッシュ 
 *------------------------------------------
 */
void push_copy (struct script_stack *stack, int pos)
{
    switch (stack->stack_data[pos].type)
    {
        case C_CONSTSTR:
            push_str (stack, C_CONSTSTR, stack->stack_data[pos].u.str);
            break;
        case C_STR:
            push_str (stack, C_STR, strdup (stack->stack_data[pos].u.str));
            break;
        default:
            push_val (stack, stack->stack_data[pos].type,
                      stack->stack_data[pos].u.num);
            break;
    }
}

/*==========================================
 * スタックからポップ 
 *------------------------------------------
 */
void pop_stack (struct script_stack *stack, int start, int end)
{
    int  i;
    for (i = start; i < end; i++)
    {
        if (stack->stack_data[i].type == C_STR)
        {
            free (stack->stack_data[i].u.str);
            stack->stack_data[i].u.str = 0;
        }
    }
    if (stack->sp > end)
    {
        memmove (&stack->stack_data[start], &stack->stack_data[end],
                 sizeof (stack->stack_data[0]) * (stack->sp - end));
    }
    stack->sp -= end - start;
}

#define BUILDIN_DEF(x,args) { buildin_ ## x , #x , args }
#define BUILDIN_DEF2(x,x2,args) { buildin_ ## x , x2 , args }
#define BUILDIN_FUNC(x) int buildin_ ## x (struct script_state* st)
#define BUILDIN_FUNCU(x) int buildin_ ## x (struct script_state* st __attribute__ ((unused)))

//
// 埋め込み関数 
//
/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(mes)
{
    clif_scriptmes (script_rid2sd (st), st->oid, script_getstr(st, 2));
    return 0;
}

BUILDIN_FUNC(mesn)
{
    char *msg = script_getstr(st, 2);
    if (!msg)
        return 0;
    char *buf = (char *) aCalloc (strlen(msg) + 3, sizeof (char));
    strcpy (buf, "[");
    strcat (buf, msg);
    strcat (buf, "]");

    clif_scriptmes (script_rid2sd (st), st->oid, buf);
    return 0;
}

BUILDIN_FUNC(mesq)
{
    char *msg = script_getstr(st, 2);
    if (!msg)
        return 0;
    char *buf = (char *) aCalloc (strlen(msg) + 3, sizeof (char));
    strcpy (buf, "\"");
    strcat (buf, msg);
    strcat (buf, "\"");

    clif_scriptmes (script_rid2sd (st), st->oid, buf);
    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(goto)
{
    int  pos;

    if (!st)
        return 0;

    if (st->stack->stack_data[st->start + 2].type != C_POS)
    {
        printf ("script: goto: not label !\n");
        st->state = END;
        return 0;
    }

    pos = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    st->pos = pos;
    st->state = GOTO;
    return 0;
}

/*==========================================
 * ユーザー定義関数の呼び出し 
 *------------------------------------------
 */
BUILDIN_FUNC(callfunc)
{
    char *scr;
    char *str = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    if (!str)
    {
        printf ("script:callfunc: function cant be null!\n");
        st->state = END;
        return 0;
    }

    if ((scr = strdb_search (script_get_userfunc_db (), str)))
    {
        int  i, j;
        for (i = st->start + 3, j = 0; i < st->end; i++, j++)
            push_copy (st->stack, i);

        push_val (st->stack, C_INT, j); // 引数の数をプッシュ 
        push_val (st->stack, C_INT, st->defsp);  // 現在の基準スタックポインタをプッシュ
        push_val (st->stack, C_INT, (int) st->script);  // 現在のスクリプトをプッシュ 
        push_val (st->stack, C_RETINFO, st->pos);       // 現在のスクリプト位置をプッシュ 

        st->pos = 0;
        st->script = scr;
        st->defsp = st->start + 4 + j;
        st->state = GOTO;
    }
    else
    {
        printf ("script:callfunc: function not found! [%s]\n", str);
        st->state = END;
    }
    return 0;
}

/*==========================================
 * サブルーティンの呼び出し 
 *------------------------------------------
 */
BUILDIN_FUNC(callsub)
{
    int  pos = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    int  i, j;
    for (i = st->start + 3, j = 0; i < st->end; i++, j++)
        push_copy (st->stack, i);

    push_val (st->stack, C_INT, j);                 // 引数の数をプッシュ 
    push_val (st->stack, C_INT, st->defsp);         // 現在の基準スタックポインタをプッシュ 
    push_val (st->stack, C_INT, (int) st->script);  // 現在のスクリプトをプッシュ 
    push_val (st->stack, C_RETINFO, st->pos);       // 現在のスクリプト位置をプッシュ 

    st->pos = pos;
    st->defsp = st->start + 4 + j;
    st->state = GOTO;
    return 0;
}

/*==========================================
 * 引数の所得 
 *------------------------------------------
 */
BUILDIN_FUNC(getarg)
{
    int  num = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    int  max, stsp;
    if (st->defsp < 4
        || st->stack->stack_data[st->defsp - 1].type != C_RETINFO)
    {
        printf ("script:getarg without callfunc or callsub!\n");
        st->state = END;
        return 0;
    }
    max = conv_num (st, &(st->stack->stack_data[st->defsp - 4]));
    stsp = st->defsp - max - 4;
    if (num >= max)
    {
        printf ("script:getarg arg1(%d) out of range(%d) !\n", num, max);
        st->state = END;
        return 0;
    }
    push_copy (st->stack, stsp + num);
    return 0;
}

/*==========================================
 * サブルーチン/ユーザー定義関数の終了 
 *------------------------------------------
 */
BUILDIN_FUNC(return)
{
    if (st->end > st->start + 2)
    {                           // 戻り値有り 
        push_copy (st->stack, st->start + 2);
    }
    st->state = RETFUNC;
    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(next)
{
    st->state = STOP;
    clif_scriptnext (script_rid2sd (st), st->oid);
    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(close)
{
    st->state = END;
    clif_scriptclose (script_rid2sd (st), st->oid);
    return 0;
}

BUILDIN_FUNC(close2)
{
    st->state = STOP;
    clif_scriptclose (script_rid2sd (st), st->oid);
    return 0;
}

BUILDIN_FUNC(close3)
{
    st->state = STOP;
    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(menu)
{
    char *buf;
    int  i, len = 0;            // [fate] len is the total # of bytes we need to transmit the string choices
    int  menu_choices = 0;
    int  finished_menu_items = 0;   // [fate] set to 1 after we hit the first empty string

    struct map_session_data *sd;

    sd = script_rid2sd (st);
    if (!sd)
    {
        st->state = END;
        return 1;
    }

    // We don't need to do this iteration if the player cancels, strictly speaking.
    for (i = st->start + 2; i < st->end; i += 2)
    {
        int  choice_len;
        conv_str (st, &(st->stack->stack_data[i]));
        choice_len = strlen (st->stack->stack_data[i].u.str);
        len += choice_len + 1;  // count # of bytes we'll need for packet.  Only used if menu_or_input = 0.

        if (choice_len && !finished_menu_items)
            ++menu_choices;
        else
            finished_menu_items = 1;
    }

    if (sd->state.menu_or_input == 0)
    {
        st->state = RERUNLINE;
        sd->state.menu_or_input = 1;

        buf = (char *) aCalloc (len + 1, sizeof (char));
        buf[0] = 0;
        for (i = st->start + 2; menu_choices > 0; i += 2, --menu_choices)
        {
            strcat (buf, st->stack->stack_data[i].u.str);
            strcat (buf, ":");
        }
        clif_scriptmenu (script_rid2sd (st), st->oid, buf);
        free (buf);
        buf = 0;
    }
    else if (sd->npc_menu == 0xff)
    {                           // cansel
        sd->state.menu_or_input = 0;
        st->state = END;
    }
    else
    {                           // goto動作 
        // ragemu互換のため 
        pc_setreg (sd, add_str ("l15"), sd->npc_menu);
        pc_setreg (sd, add_str ("@menu"), sd->npc_menu);
        sd->state.menu_or_input = 0;
        if (sd->npc_menu > 0 && sd->npc_menu <= menu_choices)
        {
            int  pos;
            if (st->stack->
                stack_data[st->start + sd->npc_menu * 2 + 1].type != C_POS)
            {
                st->state = END;
                return 0;
            }
            pos =
                conv_num (st,
                          &(st->
                            stack->stack_data[st->start + sd->npc_menu * 2 +
                                              1]));
            st->pos = pos;
            st->state = GOTO;
        }
    }
    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(rand)
{
    int  range;

    if (st->end > st->start + 3)
    {
        int  min, max;
        min = conv_num (st, &(st->stack->stack_data[st->start + 2]));
        max = conv_num (st, &(st->stack->stack_data[st->start + 3]));
        if (max < min)
        {
            int  tmp;
            tmp = min;
            min = max;
            max = tmp;
        }
        range = max - min + 1;
        push_val (st->stack, C_INT, (range <= 0 ? 0 : MRAND (range)) + min);
    }
    else
    {
        range = conv_num (st, &(st->stack->stack_data[st->start + 2]));
        push_val (st->stack, C_INT, range <= 0 ? 0 : MRAND (range));
    }
    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(pow)
{
    int  a, b;

    a = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    b = conv_num (st, &(st->stack->stack_data[st->start + 3]));

    push_val (st->stack, C_INT, (int) pow (a * 0.001, b));

    return 0;
}

/*==========================================
 * Check whether the PC is at the specified location
 *------------------------------------------
 */
BUILDIN_FUNC(isat)
{
    int  x, y;
    char *str;
    struct map_session_data *sd = script_rid2sd (st);

    str = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    x = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    y = conv_num (st, &(st->stack->stack_data[st->start + 4]));

    if (!sd)
        return 1;

    push_val (st->stack, C_INT,
              (x == sd->bl.x)
              && (y == sd->bl.y) && (!strcmp (str, map[sd->bl.m].name)));

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(warp)
{
    int  x, y;
    char *str;
    struct map_session_data *sd = script_rid2sd (st);

    if (!sd)
        return 1;

    str = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    x = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    y = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    if (strcmp (str, "Random") == 0)
        pc_randomwarp (sd, 3);
    else if (strcmp (str, "SavePoint") == 0)
    {
        if (map[sd->bl.m].flag.noreturn)    // 蝶禁止 
            return 0;

        pc_setpos (sd, sd->status.save_point.map,
                   sd->status.save_point.x, sd->status.save_point.y, 3);
    }
    else if (strcmp (str, "Save") == 0)
    {
        if (map[sd->bl.m].flag.noreturn)    // 蝶禁止 
            return 0;

        pc_setpos (sd, sd->status.save_point.map,
                   sd->status.save_point.x, sd->status.save_point.y, 3);
    }
    else
        pc_setpos (sd, str, x, y, 0);
    return 0;
}

/*==========================================
 * エリア指定ワープ 
 *------------------------------------------
 */
int buildin_areawarp_sub (struct block_list *bl, va_list ap)
{
    int  x, y;
    char *map;
    map = va_arg (ap, char *);
    x = va_arg (ap, int);
    y = va_arg (ap, int);
    if (strcmp (map, "Random") == 0)
        pc_randomwarp ((struct map_session_data *) bl, 3);
    else
        pc_setpos ((struct map_session_data *) bl, map, x, y, 0);
    return 0;
}

BUILDIN_FUNC(areawarp)
{
    int  x, y, m;
    char *str;
    char *mapname;
    int  x0, y0, x1, y1;

    mapname = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    x0 = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    y0 = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    x1 = conv_num (st, &(st->stack->stack_data[st->start + 5]));
    y1 = conv_num (st, &(st->stack->stack_data[st->start + 6]));
    str = conv_str (st, &(st->stack->stack_data[st->start + 7]));
    x = conv_num (st, &(st->stack->stack_data[st->start + 8]));
    y = conv_num (st, &(st->stack->stack_data[st->start + 9]));

    if ((m = map_mapname2mapid (mapname)) < 0)
        return 0;

    map_foreachinarea (buildin_areawarp_sub,
                       m, x0, y0, x1, y1, BL_PC, str, x, y);
    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(heal)
{
    int  hp, sp;

    hp = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    sp = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    pc_heal (script_rid2sd (st), hp, sp);
    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(itemheal)
{
    int  hp, sp;

    hp = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    sp = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    pc_itemheal (script_rid2sd (st), hp, sp);
    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(percentheal)
{
    int  hp, sp;

    hp = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    sp = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    pc_percentheal (script_rid2sd (st), hp, sp);
    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(jobchange)
{
    int  job, upper = -1;

    job = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    if (st->end > st->start + 3)
        upper = conv_num (st, &(st->stack->stack_data[st->start + 3]));

    if ((job >= 0 && job < MAX_PC_CLASS))
        pc_jobchange (script_rid2sd (st), job, upper);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(input)
{
    struct map_session_data *sd = NULL;
    int  num =
        (st->end >
         st->start + 2) ? st->stack->stack_data[st->start + 2].u.num : 0;
    char *name =
        (st->end >
         st->start + 2) ? str_buf + str_data[num & 0x00ffffff].str : "";
//  char prefix=*name;
    char postfix = name[strlen (name) - 1];

    sd = script_rid2sd (st);
    if (!sd)
        return 1;

    if (sd->state.menu_or_input)
    {
        sd->state.menu_or_input = 0;
        if (postfix == '$')
        {
            // 文字列 
            if (st->end > st->start + 2)
            {                   // 引数1個 
                set_reg (sd, num, name, (void *) sd->npc_str);
            }
            else
            {
                printf ("buildin_input: string discarded !!\n");
            }
        }
        else
        {

            //commented by Lupus (check Value Number Input fix in clif.c)
            //** Fix by fritz :X keeps people from abusing old input bugs
            if (sd->npc_amount < 0) //** If input amount is less then 0
            {
                clif_tradecancelled (sd);   // added "Deal has been cancelled" message by Valaris
                buildin_close (st); //** close
            }

            // 数値 
            if (st->end > st->start + 2)
            {                   // 引数1個 
                set_reg (sd, num, name, (void *) sd->npc_amount);
            }
            else
            {
                // ragemu互換のため 
                pc_setreg (sd, add_str ("l14"), sd->npc_amount);
            }
        }
    }
    else
    {
        st->state = RERUNLINE;
        if (postfix == '$')
            clif_scriptinputstr (sd, st->oid);
        else
            clif_scriptinput (sd, st->oid);
        sd->state.menu_or_input = 1;
    }
    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(if)
{
    int  sel, i;

    sel = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    if (!sel)
        return 0;

    // 関数名をコピー 
    push_copy (st->stack, st->start + 3);
    // 間に引数マーカを入れて 
    push_val (st->stack, C_ARG, 0);
    // 残りの引数をコピー 
    for (i = st->start + 4; i < st->end; i++)
    {
        push_copy (st->stack, i);
    }
    run_func (st);

    return 0;
}

/*==========================================
 * 変数設定 
 *------------------------------------------
 */
BUILDIN_FUNC(set)
{
    struct map_session_data *sd = NULL;
    int  num = st->stack->stack_data[st->start + 2].u.num;
    char *name = str_buf + str_data[num & 0x00ffffff].str;

    char prefix = *name;
    char postfix = name[strlen (name) - 1];

    if (st->stack->stack_data[st->start + 2].type != C_NAME)
    {
        printf ("script: buildin_set: not name\n");
        return 0;
    }

    if (prefix != '$')
        sd = script_rid2sd (st);

    if (postfix == '$')
    {
        // 文字列 
        char *str = conv_str (st, &(st->stack->stack_data[st->start + 3]));
        set_reg (sd, num, name, (void *) str);
    }
    else
    {
        // 数値 
        int  val = conv_num (st, &(st->stack->stack_data[st->start + 3]));
        set_reg (sd, num, name, (void *) val);
    }

    return 0;
}

/*==========================================
 * 配列変数設定 
 *------------------------------------------
 */
BUILDIN_FUNC(setarray)
{
    struct map_session_data *sd = NULL;
    int  num = st->stack->stack_data[st->start + 2].u.num;
    char *name = str_buf + str_data[num & 0x00ffffff].str;
    char prefix = *name;
    char postfix = name[strlen (name) - 1];
    int  i, j;

    if (prefix != '$' && prefix != '@')
    {
        printf ("buildin_setarray: illegal scope !\n");
        return 0;
    }
    if (prefix != '$')
        sd = script_rid2sd (st);

    for (j = 0, i = st->start + 3; i < st->end && j < 128; i++, j++)
    {
        void *v;
        if (postfix == '$')
            v = (void *) conv_str (st, &(st->stack->stack_data[i]));
        else
            v = (void *) conv_num (st, &(st->stack->stack_data[i]));
        set_reg (sd, num + (j << 24), name, v);
    }
    return 0;
}

/*==========================================
 * 配列変数クリア 
 *------------------------------------------
 */
BUILDIN_FUNC(cleararray)
{
    struct map_session_data *sd = NULL;
    int  num = st->stack->stack_data[st->start + 2].u.num;
    char *name = str_buf + str_data[num & 0x00ffffff].str;
    char prefix = *name;
    char postfix = name[strlen (name) - 1];
    int  sz = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    int  i;
    void *v;

    if (prefix != '$' && prefix != '@')
    {
        printf ("buildin_cleararray: illegal scope !\n");
        return 0;
    }
    if (prefix != '$')
        sd = script_rid2sd (st);

    if (postfix == '$')
        v = (void *) conv_str (st, &(st->stack->stack_data[st->start + 3]));
    else
        v = (void *) conv_num (st, &(st->stack->stack_data[st->start + 3]));

    for (i = 0; i < sz; i++)
        set_reg (sd, num + (i << 24), name, v);
    return 0;
}

/*==========================================
 * 配列変数コピー 
 *------------------------------------------
 */
BUILDIN_FUNC(copyarray)
{
    struct map_session_data *sd = NULL;
    int  num = st->stack->stack_data[st->start + 2].u.num;
    char *name = str_buf + str_data[num & 0x00ffffff].str;
    char prefix = *name;
    char postfix = name[strlen (name) - 1];
    int  num2 = st->stack->stack_data[st->start + 3].u.num;
    char *name2 = str_buf + str_data[num2 & 0x00ffffff].str;
    char prefix2 = *name2;
    char postfix2 = name2[strlen (name2) - 1];
    int  sz = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    int  i;

    if (prefix != '$' && prefix != '@' && prefix2 != '$' && prefix2 != '@')
    {
        printf ("buildin_copyarray: illegal scope !\n");
        return 0;
    }
    if ((postfix == '$' || postfix2 == '$') && postfix != postfix2)
    {
        printf ("buildin_copyarray: type mismatch !\n");
        return 0;
    }
    if (prefix != '$' || prefix2 != '$')
        sd = script_rid2sd (st);

    for (i = 0; i < sz; i++)
        set_reg (sd, num + (i << 24), name, get_val2 (st, num2 + (i << 24)));
    return 0;
}

/*==========================================
 * 配列変数のサイズ所得 
 *------------------------------------------
 */
static int getarraysize (struct script_state *st, int num, int postfix)
{
    int  i = (num >> 24), c = i;
    for (; i < 128; i++)
    {
        void *v = get_val2 (st, num + (i << 24));
        if (postfix == '$' && *((char *) v))
            c = i;
        if (postfix != '$' && (int) v)
            c = i;
    }
    return c + 1;
}

BUILDIN_FUNC(getarraysize)
{
    int  num = st->stack->stack_data[st->start + 2].u.num;
    char *name = str_buf + str_data[num & 0x00ffffff].str;
    char prefix = *name;
    char postfix = name[strlen (name) - 1];

    if (prefix != '$' && prefix != '@')
    {
        printf ("buildin_copyarray: illegal scope !\n");
        return 0;
    }

    push_val (st->stack, C_INT, getarraysize (st, num, postfix));
    return 0;
}

/*==========================================
 * 配列変数から要素削除 
 *------------------------------------------
 */
BUILDIN_FUNC(deletearray)
{
    struct map_session_data *sd = NULL;
    int  num = st->stack->stack_data[st->start + 2].u.num;
    char *name = str_buf + str_data[num & 0x00ffffff].str;
    char prefix = *name;
    char postfix = name[strlen (name) - 1];
    int  count = 1;
    int  i, sz = getarraysize (st, num, postfix) - (num >> 24) - count + 1;

    if ((st->end > st->start + 3))
        count = conv_num (st, &(st->stack->stack_data[st->start + 3]));

    if (prefix != '$' && prefix != '@')
    {
        printf ("buildin_deletearray: illegal scope !\n");
        return 0;
    }
    if (prefix != '$')
        sd = script_rid2sd (st);

    for (i = 0; i < sz; i++)
    {
        set_reg (sd, num + (i << 24), name,
                 get_val2 (st, num + ((i + count) << 24)));
    }
    for (; i < (128 - (num >> 24)); i++)
    {
        if (postfix != '$')
            set_reg (sd, num + (i << 24), name, 0);
        if (postfix == '$')
            set_reg (sd, num + (i << 24), name, "");
    }
    return 0;
}

/*==========================================
 * 指定要素を表す値(キー)を所得する 
 *------------------------------------------
 */
BUILDIN_FUNC(getelementofarray)
{
    if (st->stack->stack_data[st->start + 2].type == C_NAME)
    {
        int  i = conv_num (st, &(st->stack->stack_data[st->start + 3]));
        if (i > 127 || i < 0)
        {
            printf
                ("script: getelementofarray (operator[]): param2 illegal number %d\n",
                 i);
            push_val (st->stack, C_INT, 0);
        }
        else
        {
            push_val (st->stack, C_NAME,
                      (i << 24) | st->stack->stack_data[st->start + 2].u.num);
        }
    }
    else
    {
        printf
            ("script: getelementofarray (operator[]): param1 not name !\n");
        push_val (st->stack, C_INT, 0);
    }
    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(setlook)
{
    int  type, val;

    type = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    val = conv_num (st, &(st->stack->stack_data[st->start + 3]));

    pc_changelook (script_rid2sd (st), type, val);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(cutin)
{
    int  type;

    conv_str (st, &(st->stack->stack_data[st->start + 2]));
    type = conv_num (st, &(st->stack->stack_data[st->start + 3]));

    clif_cutin (script_rid2sd (st),
                st->stack->stack_data[st->start + 2].u.str, type);

    return 0;
}

/*==========================================
 * カードのイラストを表示する 
 *------------------------------------------
 */
BUILDIN_FUNC(cutincard)
{
    int  itemid;

    itemid = conv_num (st, &(st->stack->stack_data[st->start + 2]));

    clif_cutin (script_rid2sd (st), itemdb_search (itemid)->cardillustname,
                4);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(viewpoint)
{
    int  type, x, y, id, color;

    type = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    x = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    y = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    id = conv_num (st, &(st->stack->stack_data[st->start + 5]));
    color = conv_num (st, &(st->stack->stack_data[st->start + 6]));

    clif_viewpoint (script_rid2sd (st), st->oid, type, x, y, id, color);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(countitem)
{
    int  nameid = 0, count = 0;
    struct map_session_data *sd;

    struct script_data *data;

    sd = script_rid2sd (st);
    if (!sd)
    {
        push_val (st->stack, C_INT, 0);
        return 1;
    }

    data = &(st->stack->stack_data[st->start + 2]);
    get_val (st, data);
    if (data->type == C_STR || data->type == C_CONSTSTR)
    {
        const char *name = conv_str (st, data);
        struct item_data *item_data;
        if ((item_data = itemdb_searchname (name)) != NULL)
            nameid = item_data->nameid;
    }
    else
        nameid = conv_num (st, data);

    if (nameid >= 500)          //if no such ID then skip this iteration
    {
        int  i;
        for (i = 0; i < MAX_INVENTORY; i++)
        {
            if (sd->status.inventory[i].nameid == nameid)
                count += sd->status.inventory[i].amount;
        }
    }
    else
    {
        if (battle_config.error_log)
            printf ("wrong item ID : countitem(%i)\n", nameid);
    }
    push_val (st->stack, C_INT, count);

    return 0;
}

/*==========================================
 * 重量チェック 
 *------------------------------------------
 */
BUILDIN_FUNC(checkweight)
{
    int  nameid = 0, amount;
    struct map_session_data *sd;
    struct script_data *data;

    sd = script_rid2sd (st);
    if (!sd)
    {
        push_val (st->stack, C_INT, 0);
        return 1;
    }

    data = &(st->stack->stack_data[st->start + 2]);
    get_val (st, data);
    if (data->type == C_STR || data->type == C_CONSTSTR)
    {
        const char *name = conv_str (st, data);
        struct item_data *item_data = itemdb_searchname (name);
        if (item_data)
            nameid = item_data->nameid;
    }
    else
        nameid = conv_num (st, data);

    amount = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    if (amount <= 0 || nameid < 500)
    {                           //if get wrong item ID or amount<=0, don't count weight of non existing items
        push_val (st->stack, C_INT, 0);
    }

    if (itemdb_weight (nameid) * amount + sd->weight > sd->max_weight)
    {
        push_val (st->stack, C_INT, 0);
    }
    else
    {
        push_val (st->stack, C_INT, 1);
    }

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(getitem)
{
    int  nameid, amount, flag = 0;
    struct item item_tmp;
    struct map_session_data *sd;
    struct script_data *data;

    sd = script_rid2sd (st);
    if (!sd)
        return 1;

    data = &(st->stack->stack_data[st->start + 2]);
    get_val (st, data);
    if (data->type == C_STR || data->type == C_CONSTSTR)
    {
        const char *name = conv_str (st, data);
        struct item_data *item_data = itemdb_searchname (name);
        nameid = 727;           //Default to iten
        if (item_data != NULL)
            nameid = item_data->nameid;
    }
    else
        nameid = conv_num (st, data);

    if ((amount =
         conv_num (st, &(st->stack->stack_data[st->start + 3]))) <= 0)
    {
        return 0;               //return if amount <=0, skip the useles iteration
    }
    //Violet Box, Blue Box, etc - random item pick
    if (nameid < 0)
    {                                // ランダム 
        nameid = itemdb_searchrandomid (-nameid);
        flag = 1;
    }

    if (nameid > 0)
    {
        memset (&item_tmp, 0, sizeof (item_tmp));
        item_tmp.nameid = nameid;
        if (!flag)
            item_tmp.identify = 1;
        else
            item_tmp.identify = !itemdb_isequip3 (nameid);
        if (st->end > st->start + 5)    //アイテムを指定したIDに渡す 
            sd = map_id2sd (conv_num
                            (st, &(st->stack->stack_data[st->start + 5])));
        if (sd == NULL)         //アイテムを渡す相手がいなかったらお帰り 
            return 0;
        if ((flag = pc_additem (sd, &item_tmp, amount)))
        {
            clif_additem (sd, 0, 0, flag);
            map_addflooritem (&item_tmp, amount, sd->bl.m, sd->bl.x, sd->bl.y,
                              NULL, NULL, NULL, 0);
        }
    }

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(getitem2)
{
    int  nameid, amount, flag = 0;
    int  iden, ref, attr, c1, c2, c3, c4;
    struct item_data *item_data;
    struct item item_tmp;
    struct map_session_data *sd;
    struct script_data *data;

    sd = script_rid2sd (st);
    if (!sd)
        return 1;

    data = &(st->stack->stack_data[st->start + 2]);
    get_val (st, data);
    if (data->type == C_STR || data->type == C_CONSTSTR)
    {
        const char *name = conv_str (st, data);
        struct item_data *item_data = itemdb_searchname (name);
        nameid = 512;           //Apple item ID
        if (item_data)
            nameid = item_data->nameid;
    }
    else
        nameid = conv_num (st, data);

    amount = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    iden = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    ref = conv_num (st, &(st->stack->stack_data[st->start + 5]));
    attr = conv_num (st, &(st->stack->stack_data[st->start + 6]));
    c1 = conv_num (st, &(st->stack->stack_data[st->start + 7]));
    c2 = conv_num (st, &(st->stack->stack_data[st->start + 8]));
    c3 = conv_num (st, &(st->stack->stack_data[st->start + 9]));
    c4 = conv_num (st, &(st->stack->stack_data[st->start + 10]));
    if (st->end > st->start + 11)   //アイテムを指定したIDに渡す 
        sd = map_id2sd (conv_num
                        (st, &(st->stack->stack_data[st->start + 11])));
    if (sd == NULL)             //アイテムを渡す相手がいなかったらお帰り 
        return 0;

    if (nameid < 0)
     {                                   // ランダム 
        nameid = itemdb_searchrandomid (-nameid);
        flag = 1;
    }

    if (nameid > 0)
    {
        memset (&item_tmp, 0, sizeof (item_tmp));
        item_data = itemdb_search (nameid);
        if (item_data->type == 4 || item_data->type == 5)
        {
            if (ref > 10)
                ref = 10;
        }
        else if (item_data->type == 7)
        {
            iden = 1;
            ref = 0;
        }
        else
        {
            iden = 1;
            ref = attr = 0;
        }

        item_tmp.nameid = nameid;
        if (!flag)
            item_tmp.identify = iden;
        else if (item_data->type == 4 || item_data->type == 5)
            item_tmp.identify = 0;
        item_tmp.refine = ref;
        item_tmp.attribute = attr;
        item_tmp.card[0] = c1;
        item_tmp.card[1] = c2;
        item_tmp.card[2] = c3;
        item_tmp.card[3] = c4;
        if ((flag = pc_additem (sd, &item_tmp, amount)))
        {
            clif_additem (sd, 0, 0, flag);
            map_addflooritem (&item_tmp, amount, sd->bl.m, sd->bl.x, sd->bl.y,
                              NULL, NULL, NULL, 0);
        }
    }

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(makeitem)
{
    int  nameid, amount, flag = 0;
    int  x, y, m;
    char *mapname;
    struct item item_tmp;
    struct map_session_data *sd;
    struct script_data *data;

    sd = script_rid2sd (st);

    data = &(st->stack->stack_data[st->start + 2]);
    get_val (st, data);
    if (data->type == C_STR || data->type == C_CONSTSTR)
    {
        const char *name = conv_str (st, data);
        struct item_data *item_data = itemdb_searchname (name);
        nameid = 512;           //Apple Item ID
        if (item_data)
            nameid = item_data->nameid;
    }
    else
        nameid = conv_num (st, data);

    amount = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    mapname = conv_str (st, &(st->stack->stack_data[st->start + 4]));
    x = conv_num (st, &(st->stack->stack_data[st->start + 5]));
    y = conv_num (st, &(st->stack->stack_data[st->start + 6]));

    if (sd && strcmp (mapname, "this") == 0)
        m = sd->bl.m;
    else
        m = map_mapname2mapid (mapname);

    if (nameid < 0)
     {                           // ランダム 
        nameid = itemdb_searchrandomid (-nameid);
        flag = 1;
    }

    if (nameid > 0)
    {
        memset (&item_tmp, 0, sizeof (item_tmp));
        item_tmp.nameid = nameid;
        if (!flag)
            item_tmp.identify = 1;
        else
            item_tmp.identify = !itemdb_isequip3 (nameid);

//      clif_additem(sd,0,0,flag);
        map_addflooritem (&item_tmp, amount, m, x, y, NULL, NULL, NULL, 0);
    }

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(delitem)
{
    int  nameid = 0, amount, i;
    struct map_session_data *sd;
    struct script_data *data;

    sd = script_rid2sd (st);
    if (!sd)
        return 1;

    data = &(st->stack->stack_data[st->start + 2]);
    get_val (st, data);
    if (data->type == C_STR || data->type == C_CONSTSTR)
    {
        const char *name = conv_str (st, data);
        struct item_data *item_data = itemdb_searchname (name);
        //nameid=512;
        if (item_data)
            nameid = item_data->nameid;
    }
    else
        nameid = conv_num (st, data);

    amount = conv_num (st, &(st->stack->stack_data[st->start + 3]));

    if (nameid < 500 || amount <= 0)
    {                           //by Lupus. Don't run FOR if u got wrong item ID or amount<=0
        //printf("wrong item ID or amount<=0 : delitem %i,\n",nameid,amount);
        return 0;
    }
//    sd = script_rid2sd (st);

    for (i = 0; i < MAX_INVENTORY; i++)
    {
        if (sd->status.inventory[i].nameid <= 0
            || sd->inventory_data[i] == NULL
            || sd->inventory_data[i]->type != 7
            || sd->status.inventory[i].amount <= 0)
            continue;
    }
    int isOk = 0;

    for (i = 0; i < MAX_INVENTORY; i++)
    {
        if (sd->status.inventory[i].nameid == nameid)
        {
            if (sd->status.inventory[i].amount >= amount)
            {
                pc_delitem (sd, i, amount, 0);
                isOk = 1;
                break;
            }
            else
            {
                amount -= sd->status.inventory[i].amount;
                if (amount == 0)
                    amount = sd->status.inventory[i].amount;
                pc_delitem (sd, i, amount, 0);
                break;
            }
        }
    }

    if (!isOk && sd)
    {
        printf ("script del bug. Account=%d, item=%d\n", sd->bl.id, nameid);
        MAP_LOG ("script del bug. Account=%d, item=%d\n", sd->bl.id, nameid);
    }
    return 0;
}

/*==========================================
 * キャラ関係のパラメータ取得 
 *------------------------------------------
 */
BUILDIN_FUNC(readparam)
{
    int  type;
    struct map_session_data *sd;

    type = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    if (st->end > st->start + 3)
        sd = map_nick2sd (conv_str
                          (st, &(st->stack->stack_data[st->start + 3])));
    else
        sd = script_rid2sd (st);

    if (sd == NULL)
    {
        push_val (st->stack, C_INT, -1);
        return 0;
    }

    push_val (st->stack, C_INT, pc_readparam (sd, type));

    return 0;
}

/*==========================================
 * キャラ関係のID取得 
 *------------------------------------------
 */
BUILDIN_FUNC(getcharid)
{
    int  num;
    struct map_session_data *sd;

    num = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    if (st->end > st->start + 3)
        sd = map_nick2sd (conv_str
                          (st, &(st->stack->stack_data[st->start + 3])));
    else
        sd = script_rid2sd (st);
    if (sd == NULL)
    {
        push_val (st->stack, C_INT, -1);
        return 0;
    }
    if (num == 0)
        push_val (st->stack, C_INT, sd->status.char_id);
    if (num == 1)
        push_val (st->stack, C_INT, sd->status.party_id);
    if (num == 2)
        push_val (st->stack, C_INT, sd->status.guild_id);
    if (num == 3)
        push_val (st->stack, C_INT, sd->status.account_id);
    return 0;
}

/*==========================================
 * 指定IDのPT名取得 
 *------------------------------------------
 */
char *buildin_getpartyname_sub (int party_id)
{
    struct party *p;

    p = party_search (party_id);

    if (p != NULL)
    {
        char *buf;
        buf = (char *) aCalloc (24, sizeof (char));
        strcpy (buf, p->name);
        return buf;
    }

    return 0;
}

BUILDIN_FUNC(getpartyname)
{
    char *name;
    int  party_id;

    party_id = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    name = buildin_getpartyname_sub (party_id);
    if (name != 0)
        push_str (st->stack, C_STR, name);
    else
        push_str (st->stack, C_CONSTSTR, "null");

    return 0;
}

/*==========================================
 * 指定IDのPT人数とメンバーID取得 
 *------------------------------------------
 */
BUILDIN_FUNC(getpartymember)
{
    struct party *p;
    int  j = 0;

    p = party_search (conv_num (st, &(st->stack->stack_data[st->start + 2])));

    if (p != NULL)
    {
        int  i;
        for (i = 0; i < MAX_PARTY; i++)
        {
            if (p->member[i].account_id)
            {
//              printf("name:%s %d\n",p->member[i].name,i);
                mapreg_setregstr (add_str ("$@partymembername$") + (i << 24),
                                  p->member[i].name);
                j++;
            }
        }
    }
    mapreg_setreg (add_str ("$@partymembercount"), j);

    return 0;
}

/*==========================================
 * 指定IDのギルド名取得 
 *------------------------------------------
 */
char *buildin_getguildname_sub (int guild_id)
{
    struct guild *g = NULL;
    g = guild_search (guild_id);

    if (g != NULL)
    {
        char *buf;
        buf = (char *) aCalloc (24, sizeof (char));
        strcpy (buf, g->name);
        return buf;
    }
    return 0;
}

BUILDIN_FUNC(getguildname)
{
    char *name;
    int  guild_id = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    name = buildin_getguildname_sub (guild_id);
    if (name != 0)
        push_str (st->stack, C_STR, name);
    else
        push_str (st->stack, C_CONSTSTR, "null");
    return 0;
}

/*==========================================
 * 指定IDのGuildMaster名取得 
 *------------------------------------------
 */
char *buildin_getguildmaster_sub (int guild_id)
{
    struct guild *g = NULL;
    g = guild_search (guild_id);

    if (g != NULL)
    {
        char *buf;
        buf = (char *) aCalloc (24, sizeof (char));
        safestrncpy (buf, g->master, 24);
        return buf;
    }

    return 0;
}

BUILDIN_FUNC(getguildmaster)
{
    char *master;
    int  guild_id = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    master = buildin_getguildmaster_sub (guild_id);
    if (master != 0)
        push_str (st->stack, C_STR, master);
    else
        push_str (st->stack, C_CONSTSTR, "null");
    return 0;
}

BUILDIN_FUNC(getguildmasterid)
{
    char *master;
    struct map_session_data *sd = NULL;
    int  guild_id = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    master = buildin_getguildmaster_sub (guild_id);
    if (master != 0)
    {
        if ((sd = map_nick2sd (master)) == NULL)
        {
            push_val (st->stack, C_INT, 0);
            return 0;
        }
        push_val (st->stack, C_INT, sd->status.char_id);
    }
    else
    {
        push_val (st->stack, C_INT, 0);
    }
    return 0;
}

/*==========================================
 * キャラクタの名前 
 *------------------------------------------
 */
BUILDIN_FUNC(strcharinfo)
{
    struct map_session_data *sd;
    int  num;

    sd = script_rid2sd (st);
    if (!sd)
    {
        push_str (st->stack, C_CONSTSTR, "");
        return 1;
    }

    num = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    if (num == 0)
    {
        char *buf;
        buf = (char *) aCalloc (24, sizeof (char));
        safestrncpy (buf, sd->status.name, 24);
        push_str (st->stack, C_STR, buf);
    }
    if (num == 1)
    {
        char *buf;
        buf = buildin_getpartyname_sub (sd->status.party_id);
        if (buf != 0)
            push_str (st->stack, C_STR, buf);
        else
            push_str (st->stack, C_CONSTSTR, "");
    }
    if (num == 2)
    {
        char *buf;
        buf = buildin_getguildname_sub (sd->status.guild_id);
        if (buf != 0)
            push_str (st->stack, C_STR, buf);
        else
            push_str (st->stack, C_CONSTSTR, "");
    }

    return 0;
}

unsigned int equip[10] =
    { 0x0100, 0x0010, 0x0020, 0x0002, 0x0004, 0x0040, 0x0008, 0x0080, 0x0200,
    0x0001
};

/*==========================================
 * GetEquipID(Pos);     Pos: 1-10
 *------------------------------------------
 */
BUILDIN_FUNC(getequipid)
{
    int  i, num;
    struct map_session_data *sd;
    struct item_data *item;

    sd = script_rid2sd (st);
    if (sd == NULL)
    {
        printf ("getequipid: sd == NULL\n");
        return 0;
    }
    num = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    i = pc_checkequip (sd, equip[num - 1]);
    if (i >= 0)
    {
        item = sd->inventory_data[i];
        if (item)
            push_val (st->stack, C_INT, item->nameid);
        else
            push_val (st->stack, C_INT, 0);
    }
    else
    {
        push_val (st->stack, C_INT, -1);
    }
    return 0;
}

/*==========================================
 * 装備名文字列（精錬メニュー用） 
 *------------------------------------------
 */
BUILDIN_FUNC(getequipname)
{
    int  i, num;
    struct map_session_data *sd;
    struct item_data *item;
    char *buf;

    buf = (char *) aCalloc (64, sizeof (char));
    sd = script_rid2sd (st);
    num = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    i = pc_checkequip (sd, equip[num - 1]);
    if (i >= 0)
    {
        item = sd->inventory_data[i];
        if (item)
            sprintf (buf, "%s-[%s]", pos[num - 1], item->jname);
        else
            sprintf (buf, "%s-[%s]", pos[num - 1], pos[10]);
    }
    else
    {
        sprintf (buf, "%s-[%s]", pos[num - 1], pos[10]);
    }
    push_str (st->stack, C_STR, buf);

    return 0;
}

/*==========================================
 * getbrokenid [Valaris]
 *------------------------------------------
 */
BUILDIN_FUNC(getbrokenid)
{
    int  i, num, id = 0, brokencounter = 0;
    struct map_session_data *sd;

    sd = script_rid2sd (st);
    if (!sd)
    {
        push_val (st->stack, C_INT, 0);
        return 1;
    }

    num = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    for (i = 0; i < MAX_INVENTORY; i++)
    {
        if (sd->status.inventory[i].broken == 1)
        {
            brokencounter++;
            if (num == brokencounter)
            {
                id = sd->status.inventory[i].nameid;
                break;
            }
        }
    }

    push_val (st->stack, C_INT, id);

    return 0;
}

/*==========================================
 * repair [Valaris]
 *------------------------------------------
 */
BUILDIN_FUNC(repair)
{
    int  i, num;
    int  repaircounter = 0;
    struct map_session_data *sd;

    sd = script_rid2sd (st);
    if (!sd)
        return 1;

    num = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    for (i = 0; i < MAX_INVENTORY; i++)
    {
        if (sd->status.inventory[i].broken == 1)
        {
            repaircounter++;
            if (num == repaircounter)
            {
                sd->status.inventory[i].broken = 0;
                clif_equiplist (sd);
                clif_produceeffect (sd, 0, sd->status.inventory[i].nameid);
                clif_misceffect (&sd->bl, 3);
                clif_displaymessage (sd->fd, "Item has been repaired.");
                break;
            }
        }
    }

    return 0;
}

/*==========================================
 * 装備チェック 
 *------------------------------------------
 */
BUILDIN_FUNC(getequipisequiped)
{
    int  i, num;
    struct map_session_data *sd;

    num = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    sd = script_rid2sd (st);
    i = pc_checkequip (sd, equip[num - 1]);
    if (i >= 0)
    {
        push_val (st->stack, C_INT, 1);
    }
    else
    {
        push_val (st->stack, C_INT, 0);
    }

    return 0;
}

/*==========================================
 * 装備品精錬可能チェック 
 *------------------------------------------
 */
BUILDIN_FUNC(getequipisenableref)
{
    int  i, num;
    struct map_session_data *sd;

    num = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    sd = script_rid2sd (st);
    if (!sd)
    {
        push_val (st->stack, C_INT, 0);
        return 1;
    }

    i = pc_checkequip (sd, equip[num - 1]);
    if (i >= 0 && num < 7 && sd->inventory_data[i]
        && (num != 1 || sd->inventory_data[i]->def > 1
            || (sd->inventory_data[i]->def == 1
                && sd->inventory_data[i]->equip_script == NULL)
            || (sd->inventory_data[i]->def <= 0
                && sd->inventory_data[i]->equip_script != NULL)))
    {
        push_val (st->stack, C_INT, 1);
    }
    else
    {
        push_val (st->stack, C_INT, 0);
    }

    return 0;
}

/*==========================================
 * 装備品鑑定チェック 
 *------------------------------------------
 */
BUILDIN_FUNC(getequipisidentify)
{
    int  i, num;
    struct map_session_data *sd;

    num = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    if (num < 1 || num > 10)
    {
        push_val (st->stack, C_INT, 0);
        return 1;
    }

    sd = script_rid2sd (st);
    if (!sd)
    {
        push_val (st->stack, C_INT, 0);
        return 1;
    }

    i = pc_checkequip (sd, equip[num - 1]);
    if (i >= 0)
        push_val (st->stack, C_INT, sd->status.inventory[i].identify);
    else
        push_val (st->stack, C_INT, 0);

    return 0;
}

/*==========================================
 * 装備品精錬度 
 *------------------------------------------
 */
BUILDIN_FUNC(getequiprefinerycnt)
{
    int  i, num;
    struct map_session_data *sd;

    num = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    sd = script_rid2sd (st);
    i = pc_checkequip (sd, equip[num - 1]);
    if (i >= 0)
        push_val (st->stack, C_INT, sd->status.inventory[i].refine);
    else
        push_val (st->stack, C_INT, 0);

    return 0;
}

/*==========================================
 * 装備品武器LV 
 *------------------------------------------
 */
BUILDIN_FUNC(getequipweaponlv)
{
    int  i, num;
    struct map_session_data *sd;

    num = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    if (num < 1 || num > 10)
    {
        push_val (st->stack, C_INT, 0);
        return 1;
    }

    sd = script_rid2sd (st);
    if (!sd)
    {
        push_val (st->stack, C_INT, 0);
        return 1;
    }

    i = pc_checkequip (sd, equip[num - 1]);
    if (i >= 0 && sd->inventory_data[i])
        push_val (st->stack, C_INT, sd->inventory_data[i]->wlv);
    else
        push_val (st->stack, C_INT, 0);

    return 0;
}

/*==========================================
 * 装備品精錬成功率 
 *------------------------------------------
 */
BUILDIN_FUNC(getequippercentrefinery)
{
    int  i, num;
    struct map_session_data *sd;

    num = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    if (num < 1 || num > 10)
    {
        push_val (st->stack, C_INT, 0);
        return 1;
    }

    sd = script_rid2sd (st);
    if (!sd)
    {
        push_val (st->stack, C_INT, 0);
        return 1;
    }

    i = pc_checkequip (sd, equip[num - 1]);
    if (i >= 0)
        push_val (st->stack, C_INT,
                  pc_percentrefinery (sd, &sd->status.inventory[i]));
    else
        push_val (st->stack, C_INT, 0);

    return 0;
}

/*==========================================
 * 精錬成功 
 *------------------------------------------
 */
BUILDIN_FUNC(successrefitem)
{
    int  i, num;
    struct map_session_data *sd;

    num = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    if (num < 1 || num > 10)
        return 1;

    sd = script_rid2sd (st);
    if (!sd)
        return 1;

    i = pc_checkequip (sd, equip[num - 1]);
    if (i >= 0)
    {
        int  ep;
        ep = sd->status.inventory[i].equip;

        sd->status.inventory[i].refine++;
        pc_unequipitem (sd, i, 0);
        clif_refine (sd->fd, sd, 0, i, sd->status.inventory[i].refine);
        clif_delitem (sd, i, 1);
        clif_additem (sd, i, 1, 0);
        pc_equipitem (sd, i, ep);
        clif_misceffect (&sd->bl, 3);
    }

    return 0;
}

/*==========================================
 * 精錬失敗 
 *------------------------------------------
 */
BUILDIN_FUNC(failedrefitem)
{
    int  i, num;
    struct map_session_data *sd;

    num = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    if (num < 1 || num > 10)
        return 1;

    sd = script_rid2sd (st);
    if (!sd)
        return 1;

    i = pc_checkequip (sd, equip[num - 1]);
    if (i >= 0)
    {
        sd->status.inventory[i].refine = 0;
        pc_unequipitem (sd, i, 0);
        // 精錬失敗エフェクトのパケット 
        clif_refine (sd->fd, sd, 1, i, sd->status.inventory[i].refine);
        pc_delitem (sd, i, 1, 0);
        // 他の人にも失敗を通知 
        clif_misceffect (&sd->bl, 2);
    }

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(statusup)
{
    int  type;
    struct map_session_data *sd;

    type = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    sd = script_rid2sd (st);
    pc_statusup (sd, type);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(statusup2)
{
    int  type, val;
    struct map_session_data *sd;

    type = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    val = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    sd = script_rid2sd (st);
    pc_statusup2 (sd, type, val);

    return 0;
}

/*==========================================
 * 装備品による能力値ボーナス 
 *------------------------------------------
 */
BUILDIN_FUNC(bonus)
{
    int  type, val;
    struct map_session_data *sd;

    type = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    val = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    sd = script_rid2sd (st);
    pc_bonus (sd, type, val);

    return 0;
}

/*==========================================
 * 装備品による能力値ボーナス 
 *------------------------------------------
 */
BUILDIN_FUNC(bonus2)
{
    int  type, type2, val;
    struct map_session_data *sd;

    type = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    type2 = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    val = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    sd = script_rid2sd (st);
    pc_bonus2 (sd, type, type2, val);

    return 0;
}

/*==========================================
 * 装備品による能力値ボーナス 
 *------------------------------------------
 */
BUILDIN_FUNC(bonus3)
{
    int  type, type2, type3, val;
    struct map_session_data *sd;

    type = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    type2 = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    type3 = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    val = conv_num (st, &(st->stack->stack_data[st->start + 5]));
    sd = script_rid2sd (st);
    pc_bonus3 (sd, type, type2, type3, val);

    return 0;
}

/*==========================================
 * スキル所得 
 *------------------------------------------
 */
BUILDIN_FUNC(skill)
{
    int  id, level, flag = 1;
    struct map_session_data *sd;

    id = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    level = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    if (st->end > st->start + 4)
        flag = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    sd = script_rid2sd (st);
    pc_skill (sd, id, level, flag);
    clif_skillinfoblock (sd);

    return 0;
}

/*==========================================
 * [Fate] Sets the skill level permanently
 *------------------------------------------
 */
BUILDIN_FUNC(setskill)
{
    int  id, level;
    struct map_session_data *sd;

    id = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    if (id < 0 || id >= MAX_SKILL)
        return 1;

    level = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    sd = script_rid2sd (st);

    sd->status.skill[id].id = level ? id : 0;
    sd->status.skill[id].lv = level;
    clif_skillinfoblock (sd);
    return 0;
}

/*==========================================
 * ギルドスキル取得 
 *------------------------------------------
 */
BUILDIN_FUNC(guildskill)
{
    int  id, level;
    struct map_session_data *sd;
    int  i = 0;

    id = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    level = conv_num (st, &(st->stack->stack_data[st->start + 3]));
//  if( st->end>st->start+4 )
//      flag=conv_num(st,&(st->stack->stack_data[st->start+4]) );
    sd = script_rid2sd (st);
    for (i = 0; i < level; i++)
        guild_skillup (sd, id);

    return 0;
}

/*==========================================
 * スキルレベル所得 
 *------------------------------------------
 */
BUILDIN_FUNC(getskilllv)
{
    int  id = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    push_val (st->stack, C_INT, pc_checkskill (script_rid2sd (st), id));
    return 0;
}

/*==========================================
 * getgdskilllv(Guild_ID, Skill_ID);
 * skill_id = 10000 : GD_APPROVAL
 *            10001 : GD_KAFRACONTACT
 *            10002 : GD_GUARDIANRESEARCH
 *            10003 : GD_CHARISMA
 *            10004 : GD_EXTENSION
 *------------------------------------------
 */
BUILDIN_FUNC(getgdskilllv)
{
    int  guild_id = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    int  skill_id = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    struct guild *g = guild_search (guild_id);
    push_val (st->stack, C_INT,
              (g == NULL) ? -1 : guild_checkskill (g, skill_id));
    return 0;
/*
	struct map_session_data *sd=NULL;
	struct guild *g=NULL;
	int skill_id;

	skill_id=conv_num(st,& (st->stack->stack_data[st->start+2]));
	sd=script_rid2sd(st);
	if(sd && sd->status.guild_id > 0) g=guild_search(sd->status.guild_id);
	if(sd && g) {
		push_val(st->stack,C_INT, guild_checkskill(g,skill_id+9999) );
	} else {
		push_val(st->stack,C_INT,-1);
	}
	return 0;
*/
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(basicskillcheck)
{
    push_val (st->stack, C_INT, battle_config.basic_skill_check);
    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(getgmlevel)
{
    push_val (st->stack, C_INT, pc_isGM (script_rid2sd (st)));
    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(end)
{
    st->state = END;
    return 0;
}

/*==========================================
 * [Freeyorp] Return the current opt2
 *------------------------------------------
 */

BUILDIN_FUNC(getopt2)
{
    struct map_session_data *sd;

    sd = script_rid2sd (st);
    if (!sd)
    {
        push_val (st->stack, C_INT, 0);
        return 1;
    }

    push_val (st->stack, C_INT, sd->opt2);

    return 0;
}

/*==========================================
 * [Freeyorp] Sets opt2
 *------------------------------------------
 */

BUILDIN_FUNC(setopt2)
{
    int  new;
    struct map_session_data *sd;

    new = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    sd = script_rid2sd (st);
    if (!sd)
        return 1;

    if (!(new ^ sd->opt2))
        return 0;
    sd->opt2 = new;
    clif_changeoption (&sd->bl);
    pc_calcstatus (sd, 0);

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(checkoption)
{
    int  type;
    struct map_session_data *sd;

    type = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    sd = script_rid2sd (st);
    if (!sd)
    {
        push_val (st->stack, C_INT, 0);
        return 1;
    }

    if (sd->status.option & type)
    {
        push_val (st->stack, C_INT, 1);
    }
    else
    {
        push_val (st->stack, C_INT, 0);
    }

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(setoption)
{
    int  type;
    struct map_session_data *sd;

    type = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    sd = script_rid2sd (st);
    pc_setoption (sd, type);

    return 0;
}

/*==========================================
 * Checkcart [Valaris]
 *------------------------------------------
 */

BUILDIN_FUNC(checkcart)
{
    struct map_session_data *sd;

    sd = script_rid2sd (st);

    if (pc_iscarton (sd))
    {
        push_val (st->stack, C_INT, 1);
    }
    else
    {
        push_val (st->stack, C_INT, 0);
    }
    return 0;
}

/*==========================================
 * Gain guild exp [Celest]
 *------------------------------------------*/
BUILDIN_FUNC(guildgetexp)
{
    TBL_PC* sd;
    int exp;

    sd = script_rid2sd(st);
    if (sd == NULL)
        return 0;

    exp = script_getnum(st, 2);
    if (exp < 0)
        return 0;
    if(sd && sd->status.guild_id > 0)
        guild_getexp (sd, exp);

    return 0;
}

/*==========================================
 * Allows player to write NPC logs (i.e. Bank NPC, etc) [Lupus]
 *------------------------------------------*/
BUILDIN_FUNCU(logmes)
{
//  const char *str;
//  TBL_PC* sd;

//    if (log_config.npc <= 0)
//        return 0;

//  sd = script_rid2sd(st);
//  if (sd == NULL)
//      return 1;

//  str = script_getstr(st, 2);
//  log_npc(sd, str);
    return 0;
}

BUILDIN_FUNC(summon)
{
    int _class, timeout = 0;
    const char *event = "";
    TBL_PC *sd;
    struct mob_data *md;
    int tick = gettick();

    sd = script_rid2sd(st);
    if (!sd)
        return 0;
//    str = script_getstr(st, 2);
    _class = script_getnum(st, 3);
    if (script_hasdata(st, 4))
        timeout = script_getnum(st, 4);
    if (script_hasdata(st, 5))
    {
        event = script_getstr(st, 5);
        check_event(st, event);
    }

    clif_skill_poseffect(&sd->bl, AM_CALLHOMUN, 1, sd->bl.x, sd->bl.y, tick);

    int mob_id = mob_once_spawn (sd, map[sd->bl.m].name, sd->bl.x, sd->bl.y, "--ja--",
                                 _class, 1, event);

    md = (struct mob_data *) map_id2bl (mob_id);

//    md = mob_once_spawn_sub(&sd->bl, sd->bl.m, sd->bl.x, sd->bl.y, str, _class, event);
    if (md)
    {
        md->mode = mob_db[md->class].mode | 0x04;
        md->master_id = sd->bl.id;
        md->state.special_mob_ai = 1;
        md->deletetimer = add_timer(tick + (timeout > 0 ? timeout * 1000 : 60000), mob_timer_delete, md->bl.id, 0);
//        mob_spawn (md); //Now it is ready for spawning.
        clif_misceffect(&md->bl, 344);
//        sc_start4(&md->bl, SC_MODECHANGE, 100, 1, 0, MD_AGGRESSIVE, 0, 60000);
    }
    return 0;
}

/*==========================================
 * Changes the guild master of a guild [Skotlex]
 *------------------------------------------*/
BUILDIN_FUNC(guildchangegm)
{
    TBL_PC *sd;
    int guild_id;
    const char *name;

    guild_id = script_getnum(st, 2);
    name = script_getstr(st, 3);
    sd = map_nick2sd(name);

    if (!sd)
        script_pushint(st, 0);
    else
        script_pushint(st, guild_gm_change(guild_id, sd));

    return 0;
}

/*==========================================
 * カートを付ける 
 *------------------------------------------
 */
BUILDIN_FUNC(setcart)
{
    struct map_session_data *sd;

    sd = script_rid2sd (st);
    pc_setcart (sd, 1);

    return 0;
}

/*==========================================
 * checkfalcon [Valaris]
 *------------------------------------------
 */

BUILDIN_FUNC(checkfalcon)
{
    struct map_session_data *sd;

    sd = script_rid2sd (st);

    if (pc_isfalcon (sd))
    {
        push_val (st->stack, C_INT, 1);
    }
    else
    {
        push_val (st->stack, C_INT, 0);
    }

    return 0;
}

/*==========================================
 * 鷹を付ける 
 *------------------------------------------
 */
BUILDIN_FUNC(setfalcon)
{
    struct map_session_data *sd;

    sd = script_rid2sd (st);
    pc_setfalcon (sd);

    return 0;
}

/*==========================================
 * Checkcart [Valaris]
 *------------------------------------------
 */

BUILDIN_FUNC(checkriding)
{
    struct map_session_data *sd;

    sd = script_rid2sd (st);

    if (pc_isriding (sd))
    {
        push_val (st->stack, C_INT, 1);
    }
    else
    {
        push_val (st->stack, C_INT, 0);
    }

    return 0;
}

/*==========================================
 * ペコペコ乗り 
 *------------------------------------------
 */
BUILDIN_FUNC(setriding)
{
    struct map_session_data *sd;

    sd = script_rid2sd (st);
    pc_setriding (sd);

    return 0;
}

/*==========================================
 * セーブポイントの保存 
 *------------------------------------------
 */
BUILDIN_FUNC(savepoint)
{
    int  x, y;
    char *str;

    str = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    x = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    y = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    pc_setsavepoint (script_rid2sd (st), str, x, y);
    return 0;
}

/*==========================================
 * gettimetick(type)
 *
 * type The type of time measurement.
 *  Specify 0 for the system tick, 1 for
 *  seconds elapsed today, or 2 for seconds
 *  since Unix epoch. Defaults to 0 for any
 *  other value.
 *------------------------------------------
 */
BUILDIN_FUNC(gettimetick)   /* Asgard Version */
{
    int  type;
    type = conv_num (st, &(st->stack->stack_data[st->start + 2]));

    switch (type)
    {
        /* Number of seconds elapsed today (0-86399, 00:00:00-23:59:59). */
        case 1:
        {
            time_t timer;
            struct tm *t;

            time (&timer);
            t = gmtime (&timer);
            push_val (st->stack, C_INT,
                      ((t->tm_hour) * 3600 + (t->tm_min) * 60 + t->tm_sec));
            break;
        }
        /* Seconds since Unix epoch. */
        case 2:
            push_val (st->stack, C_INT, (int) time (NULL));
            break;
        /* System tick (unsigned int, and yes, it will wrap). */
        case 0:
        default:
            push_val (st->stack, C_INT, gettick ());
            break;
    }
    return 0;
}

/*==========================================
 * GetTime(Type);
 * 1: Sec     2: Min     3: Hour
 * 4: WeekDay     5: MonthDay     6: Month
 * 7: Year
 *------------------------------------------
 */
BUILDIN_FUNC(gettime)   /* Asgard Version */
{
    int  type;
    time_t timer;
    struct tm *t;

    type = conv_num (st, &(st->stack->stack_data[st->start + 2]));

    time (&timer);
    t = gmtime (&timer);

    switch (type)
    {
        case 1:                //Sec(0~59)
            push_val (st->stack, C_INT, t->tm_sec);
            break;
        case 2:                //Min(0~59)
            push_val (st->stack, C_INT, t->tm_min);
            break;
        case 3:                //Hour(0~23)
            push_val (st->stack, C_INT, t->tm_hour);
            break;
        case 4:                //WeekDay(0~6)
            push_val (st->stack, C_INT, t->tm_wday);
            break;
        case 5:                //MonthDay(01~31)
            push_val (st->stack, C_INT, t->tm_mday);
            break;
        case 6:                //Month(01~12)
            push_val (st->stack, C_INT, t->tm_mon + 1);
            break;
        case 7:                //Year(20xx)
            push_val (st->stack, C_INT, t->tm_year + 1900);
            break;
        default:               //(format error)
            push_val (st->stack, C_INT, -1);
            break;
    }
    return 0;
}

/*==========================================
 * GetTimeStr("TimeFMT", Length);
 *------------------------------------------
 */
BUILDIN_FUNC(gettimestr)
{
    char *tmpstr;
    char *fmtstr;
    int  maxlen;
    time_t now = time (NULL);

    fmtstr = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    maxlen = conv_num (st, &(st->stack->stack_data[st->start + 3]));

    tmpstr = (char *) aCalloc (maxlen + 1, sizeof (char));
    strftime (tmpstr, maxlen, fmtstr, gmtime (&now));
    tmpstr[maxlen] = '\0';

    push_str (st->stack, C_STR, tmpstr);
    return 0;
}

/*==========================================
 * カプラ倉庫を開く 
 *------------------------------------------
 */
BUILDIN_FUNC(openstorage)
{
//  int sync = 0;
//  if (st->end >= 3) sync = conv_num(st,& (st->stack->stack_data[st->start+2]));
    struct map_session_data *sd = script_rid2sd (st);

    if (!sd)
        return 1;

//  if (sync) {
    st->state = STOP;
//+++ why set this flag?
    sd->npc_flags.storage = 1;
//  } else st->state = END;

    storage_storageopen (sd);
    return 0;
}

BUILDIN_FUNC(guildopenstorage)
{
    struct map_session_data *sd = script_rid2sd (st);
    int  ret;

    st->state = STOP;

//+++ why no set storage flag?

    ret = storage_guild_storageopen (sd);
    push_val (st->stack, C_INT, ret);
    return 0;
}

/*==========================================
 * アイテムによるスキル発動 
 *------------------------------------------
 */
BUILDIN_FUNC(itemskill)
{
    int  id, lv;
    char *str;
    struct map_session_data *sd = script_rid2sd (st);

    id = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    lv = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    str = conv_str (st, &(st->stack->stack_data[st->start + 4]));

    // 詠唱中にスキルアイテムは使用できない 
    if (sd->skilltimer != -1)
        return 0;

//+++ may be need chaeck skillitem?

    sd->skillitem = id;
    sd->skillitemlv = lv;
    clif_item_skill (sd, id, lv, str);
    return 0;
}

/*==========================================
 * NPCで経験値上げる 
 *------------------------------------------
 */
BUILDIN_FUNC(getexp)
{
    struct map_session_data *sd = script_rid2sd (st);
    int  base = 0, job = 0;

    base = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    job = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    if (base < 0 || job < 0)
        return 0;
    if (sd)
        pc_gainexp_reason (sd, base, job, PC_GAINEXP_REASON_SCRIPT);

    return 0;
}

/*==========================================
 * モンスター発生 
 *------------------------------------------
 */
BUILDIN_FUNC(monster)
{
    int  class, amount, x, y;
    char *str, *map, *event = "";

    map = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    x = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    y = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    str = conv_str (st, &(st->stack->stack_data[st->start + 5]));
    class = conv_num (st, &(st->stack->stack_data[st->start + 6]));
    amount = conv_num (st, &(st->stack->stack_data[st->start + 7]));
    if (st->end > st->start + 8)
        event = conv_str (st, &(st->stack->stack_data[st->start + 8]));

    mob_once_spawn (map_id2sd (st->rid), map, x, y, str, class, amount,
                    event);
    return 0;
}

/*==========================================
 * モンスター発生 
 *------------------------------------------
 */
BUILDIN_FUNC(areamonster)
{
    int  class, amount, x0, y0, x1, y1;
    char *str, *map, *event = "";

    map = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    x0 = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    y0 = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    x1 = conv_num (st, &(st->stack->stack_data[st->start + 5]));
    y1 = conv_num (st, &(st->stack->stack_data[st->start + 6]));
    str = conv_str (st, &(st->stack->stack_data[st->start + 7]));
    class = conv_num (st, &(st->stack->stack_data[st->start + 8]));
    amount = conv_num (st, &(st->stack->stack_data[st->start + 9]));
    if (st->end > st->start + 10)
        event = conv_str (st, &(st->stack->stack_data[st->start + 10]));

    mob_once_spawn_area (map_id2sd (st->rid), map, x0, y0, x1, y1, str, class,
                         amount, event);
    return 0;
}

/*==========================================
 * モンスター削除 
 *------------------------------------------
 */
int buildin_killmonster_sub (struct block_list *bl, va_list ap)
{
    if (!bl || !ap)
        return 0;

    char *event = va_arg (ap, char *);
    if (!event)
        return 0;

    int  allflag = va_arg (ap, int);

    if (!allflag)
    {
        if (strcmp (event, ((struct mob_data *) bl)->npc_event) == 0)
            mob_delete ((struct mob_data *) bl);
        return 0;
    }
    else if (allflag)
    {
        if (((struct mob_data *) bl)->spawndelay1 == -1
            && ((struct mob_data *) bl)->spawndelay2 == -1)
            mob_delete ((struct mob_data *) bl);
        return 0;
    }
    return 0;
}

BUILDIN_FUNC(killmonster)
{
    char *mapname, *event;
    int  m, allflag = 0;
    mapname = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    event = conv_str (st, &(st->stack->stack_data[st->start + 3]));
    if (strcmp (event, "All") == 0)
        allflag = 1;

    if ((m = map_mapname2mapid (mapname)) < 0)
        return 0;
    map_foreachinarea (buildin_killmonster_sub,
                       m, 0, 0, map[m].xs, map[m].ys, BL_MOB, event, allflag);
    return 0;
}

int buildin_killmonsterall_sub (struct block_list *bl,
                                va_list ap __attribute__ ((unused)))
{
    mob_delete ((struct mob_data *) bl);
    return 0;
}

BUILDIN_FUNC(killmonsterall)
{
    char *mapname;
    int  m;
    mapname = conv_str (st, &(st->stack->stack_data[st->start + 2]));

    if ((m = map_mapname2mapid (mapname)) < 0)
        return 0;
    map_foreachinarea (buildin_killmonsterall_sub,
                       m, 0, 0, map[m].xs, map[m].ys, BL_MOB);
    return 0;
}

/*==========================================
 * イベント実行 
 *------------------------------------------
 */
BUILDIN_FUNC(doevent)
{
    struct map_session_data *sd = script_rid2sd (st);
    if (!sd)
        return 0;

    char *event;
    event = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    npc_event (map_id2sd (st->rid), event, 2);
    return 0;
}

/*==========================================
 * NPC主体イベント実行 
 *------------------------------------------
 */
BUILDIN_FUNC(donpcevent)
{
    struct map_session_data *sd = script_rid2sd (st);
    if (!sd)
        return 0;

    char *event;
    event = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    npc_event_do (event);
    return 0;
}

/*==========================================
 * イベントタイマー追加 
 *------------------------------------------
 */
BUILDIN_FUNC(addtimer)
{
    char *event;
    int  tick;
    tick = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    event = conv_str (st, &(st->stack->stack_data[st->start + 3]));
    pc_addeventtimer (script_rid2sd (st), tick, event);
    return 0;
}

/*==========================================
 * イベントタイマー削除 
 *------------------------------------------
 */
BUILDIN_FUNC(deltimer)
{
    char *event;
    event = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    pc_deleventtimer (script_rid2sd (st), event);
    return 0;
}

/*==========================================
 * イベントタイマーのカウント値追加 
 *------------------------------------------
 */
BUILDIN_FUNC(addtimercount)
{
    char *event;
    int  tick;
    event = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    tick = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    pc_addeventtimercount (script_rid2sd (st), event, tick);
    return 0;
}

/*==========================================
 * NPCタイマー初期化 
 *------------------------------------------
 */
BUILDIN_FUNC(initnpctimer)
{
    struct npc_data *nd;
    if (st->end > st->start + 2)
        nd = npc_name2id (conv_str
                          (st, &(st->stack->stack_data[st->start + 2])));
    else
        nd = (struct npc_data *) map_id2bl (st->oid);

    npc_settimerevent_tick (nd, 0);
    npc_timerevent_start (nd);
    return 0;
}

/*==========================================
 * NPCタイマー開始 
 *------------------------------------------
 */
BUILDIN_FUNC(startnpctimer)
{
    struct npc_data *nd;
    if (st->end > st->start + 2)
        nd = npc_name2id (conv_str
                          (st, &(st->stack->stack_data[st->start + 2])));
    else
        nd = (struct npc_data *) map_id2bl (st->oid);

    npc_timerevent_start (nd);
    return 0;
}

/*==========================================
 * NPCタイマー停止 
 *------------------------------------------
 */
BUILDIN_FUNC(stopnpctimer)
{
    struct npc_data *nd;
    if (st->end > st->start + 2)
        nd = npc_name2id (conv_str
                          (st, &(st->stack->stack_data[st->start + 2])));
    else
        nd = (struct npc_data *) map_id2bl (st->oid);

    npc_timerevent_stop (nd);
    return 0;
}

/*==========================================
 * NPCタイマー情報所得 
 *------------------------------------------
 */
BUILDIN_FUNC(getnpctimer)
{
    struct npc_data *nd;
    int  type = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    int  val = 0;
    if (st->end > st->start + 3)
        nd = npc_name2id (conv_str
                          (st, &(st->stack->stack_data[st->start + 3])));
    else
        nd = (struct npc_data *) map_id2bl (st->oid);

    switch (type)
    {
        case 0:
            val = npc_gettimerevent_tick (nd);
            break;
        case 1:
            if (nd)
                val = (nd->u.scr.nexttimer >= 0);
            else
            {
                push_val (st->stack, C_INT, 0);
                return 1;
            }
            break;
        case 2:
            if (nd)
                val = nd->u.scr.timeramount;
            else
            {
                push_val (st->stack, C_INT, 0);
                return 1;
            }
            break;
        default:
            break;
    }
    push_val (st->stack, C_INT, val);
    return 0;
}

/*==========================================
 * NPCタイマー値設定 
 *------------------------------------------
 */
BUILDIN_FUNC(setnpctimer)
{
    int  tick;
    struct npc_data *nd;
    tick = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    if (st->end > st->start + 3)
        nd = npc_name2id (conv_str
                          (st, &(st->stack->stack_data[st->start + 3])));
    else
        nd = (struct npc_data *) map_id2bl (st->oid);

    npc_settimerevent_tick (nd, tick);
    return 0;
}

/*==========================================
 * 天の声アナウンス 
 *------------------------------------------
 */
BUILDIN_FUNC(announce)
{
    char *str;
    int  flag;
    str = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    flag = conv_num (st, &(st->stack->stack_data[st->start + 3]));

    if (flag & 0x0f)
    {
        struct block_list *bl = (flag & 0x08) ? map_id2bl (st->oid) :
            (struct block_list *) script_rid2sd (st);
        clif_GMmessage (bl, str, strlen (str) + 1, flag);
    }
    else
        intif_GMmessage (str, strlen (str) + 1, flag);
    return 0;
}

/*==========================================
 * 天の声アナウンス（特定マップ） 
 *------------------------------------------
 */
int buildin_mapannounce_sub (struct block_list *bl, va_list ap)
{
    if (!ap)
        return 0;

    char *str;
    int  len, flag;
    str = va_arg (ap, char *);
    len = va_arg (ap, int);
    flag = va_arg (ap, int);
    clif_GMmessage (bl, str, len, flag | 3);
    return 0;
}

BUILDIN_FUNC(mapannounce)
{
    char *mapname, *str;
    int  flag, m;

    mapname = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    str = conv_str (st, &(st->stack->stack_data[st->start + 3]));
    flag = conv_num (st, &(st->stack->stack_data[st->start + 4]));

    if ((m = map_mapname2mapid (mapname)) < 0)
        return 0;
    map_foreachinarea (buildin_mapannounce_sub,
                       m, 0, 0, map[m].xs, map[m].ys, BL_PC, str,
                       strlen (str) + 1, flag & 0x10);
    return 0;
}

/*==========================================
 * 天の声アナウンス（特定エリア） 
 *------------------------------------------
 */
BUILDIN_FUNC(areaannounce)
{
    char *map, *str;
    int  flag, m;
    int  x0, y0, x1, y1;

    map = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    x0 = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    y0 = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    x1 = conv_num (st, &(st->stack->stack_data[st->start + 5]));
    y1 = conv_num (st, &(st->stack->stack_data[st->start + 6]));
    str = conv_str (st, &(st->stack->stack_data[st->start + 7]));
    flag = conv_num (st, &(st->stack->stack_data[st->start + 8]));

    if ((m = map_mapname2mapid (map)) < 0)
        return 0;

    map_foreachinarea (buildin_mapannounce_sub,
                       m, x0, y0, x1, y1, BL_PC, str, strlen (str) + 1,
                       flag & 0x10);
    return 0;
}

/*==========================================
 * ユーザー数所得 
 *------------------------------------------
 */
BUILDIN_FUNC(getusers)
{
    int  flag = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    struct block_list *bl = map_id2bl ((flag & 0x08) ? st->oid : st->rid);
    int  val = 0;
    switch (flag & 0x07)
    {
        case 0:
            if (!bl)
            {
                push_val (st->stack, C_INT, 0);
                return 1;
            }
            val = map[bl->m].users;
            break;
        case 1:
            val = map_getusers ();
            break;
        default:
            break;
    }
    push_val (st->stack, C_INT, val);
    return 0;
}

/*==========================================
 * マップ指定ユーザー数所得 
 *------------------------------------------
 */
BUILDIN_FUNC(getmapusers)
{
    char *str;
    int  m;
    str = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    if ((m = map_mapname2mapid (str)) < 0)
    {
        push_val (st->stack, C_INT, -1);
        return 0;
    }
    push_val (st->stack, C_INT, map[m].users);
    return 0;
}

/*==========================================
 * エリア指定ユーザー数所得 
 *------------------------------------------
 */
int buildin_getareausers_sub (struct block_list *bl __attribute__ ((unused)),
                              va_list ap)
{
    if (!ap)
        return 0;
    int *users = va_arg (ap, int *);
    if (users)
        (*users)++;

    return 0;
}

BUILDIN_FUNC(getareausers)
{
    char *str;
    int  m, x0, y0, x1, y1, users = 0;
    str = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    x0 = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    y0 = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    x1 = conv_num (st, &(st->stack->stack_data[st->start + 5]));
    y1 = conv_num (st, &(st->stack->stack_data[st->start + 6]));
    if ((m = map_mapname2mapid (str)) < 0)
    {
        push_val (st->stack, C_INT, -1);
        return 0;
    }
    map_foreachinarea (buildin_getareausers_sub,
                       m, x0, y0, x1, y1, BL_PC, &users);
    push_val (st->stack, C_INT, users);
    return 0;
}

/*==========================================
 * エリア指定ドロップアイテム数所得 
 *------------------------------------------
 */
int buildin_getareadropitem_sub (struct block_list *bl, va_list ap)
{
    if (!ap || !bl)
        return 0;

    int  item = va_arg (ap, int);
    int *amount = va_arg (ap, int *);
    if (!amount)
        return 0;

    struct flooritem_data *drop = (struct flooritem_data *) bl;

    if (drop->item_data.nameid == item)
        (*amount) += drop->item_data.amount;

    return 0;
}

BUILDIN_FUNC(getareadropitem)
{
    char *str;
    int  m, x0, y0, x1, y1, item, amount = 0;
    struct script_data *data;

    str = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    x0 = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    y0 = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    x1 = conv_num (st, &(st->stack->stack_data[st->start + 5]));
    y1 = conv_num (st, &(st->stack->stack_data[st->start + 6]));

    data = &(st->stack->stack_data[st->start + 7]);
    get_val (st, data);
    if (data->type == C_STR || data->type == C_CONSTSTR)
    {
        const char *name = conv_str (st, data);
        struct item_data *item_data = itemdb_searchname (name);
        item = 512;
        if (item_data)
            item = item_data->nameid;
    }
    else
        item = conv_num (st, data);

    if ((m = map_mapname2mapid (str)) < 0)
    {
        push_val (st->stack, C_INT, -1);
        return 0;
    }
    map_foreachinarea (buildin_getareadropitem_sub,
                       m, x0, y0, x1, y1, BL_ITEM, item, &amount);
    push_val (st->stack, C_INT, amount);
    return 0;
}

/*==========================================
 * NPCの有効化 
 *------------------------------------------
 */
BUILDIN_FUNC(enablenpc)
{
    char *str;
    str = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    npc_enable (str, 1);
    return 0;
}

/*==========================================
 * NPCの無効化 
 *------------------------------------------
 */
BUILDIN_FUNC(disablenpc)
{
    char *str;
    str = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    npc_enable (str, 0);
    return 0;
}

BUILDIN_FUNC(enablearena)   // Added by RoVeRT
{
    struct npc_data *nd = (struct npc_data *) map_id2bl (st->oid);
    struct chat_data *cd;

    if (nd == NULL
        || (cd = (struct chat_data *) map_id2bl (nd->chat_id)) == NULL)
        return 0;

    npc_enable (nd->name, 1);
    nd->arenaflag = 1;

    if (cd->users >= cd->trigger && cd->npc_event[0])
        npc_timer_event (cd->npc_event);

    return 0;
}

BUILDIN_FUNC(disablearena)  // Added by RoVeRT
{
    struct npc_data *nd = (struct npc_data *) map_id2bl (st->oid);
    if (nd)
        nd->arenaflag = 0;

    return 0;
}

/*==========================================
 * 隠れているNPCの表示 
 *------------------------------------------
 */
BUILDIN_FUNC(hideoffnpc)
{
    char *str;
    str = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    npc_enable (str, 2);
    return 0;
}

/*==========================================
 * NPCをハイディング 
 *------------------------------------------
 */
BUILDIN_FUNC(hideonnpc)
{
    char *str;
    str = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    npc_enable (str, 4);
    return 0;
}

/*==========================================
 * 状態異常にかかる 
 *------------------------------------------
 */
BUILDIN_FUNC(sc_start)
{
    struct block_list *bl;
    int  type, tick, val1;
    type = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    tick = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    val1 = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    if (st->end > st->start + 5)        //指定したキャラを状態異常にする 
        bl = map_id2bl (conv_num
                        (st, &(st->stack->stack_data[st->start + 5])));
    else
        bl = map_id2bl (st->rid);
    if (!bl)
        return 0;

    if (bl->type == BL_PC
        && ((struct map_session_data *) bl)->state.potionpitcher_flag)
        bl = map_id2bl (((struct map_session_data *) bl)->skilltarget);
    skill_status_change_start (bl, type, val1, 0, 0, 0, tick, 0);
    return 0;
}

BUILDIN_FUNC(sc_startv2)
{
    struct block_list *bl;
    int  type, tick, val1, val2;
    type = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    tick = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    val1 = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    val2 = conv_num (st, &(st->stack->stack_data[st->start + 5]));
    if (st->end > st->start + 6)    //指定したキャラを状態異常にする 
        bl = map_id2bl (conv_num
                        (st, &(st->stack->stack_data[st->start + 6])));
    else
        bl = map_id2bl (st->rid);
    if (!bl)
        return 0;

    if (bl->type == BL_PC
        && ((struct map_session_data *) bl)->state.potionpitcher_flag)
        bl = map_id2bl (((struct map_session_data *) bl)->skilltarget);
    skill_status_change_start (bl, type, val1, val2, 0, 0, tick, 0);
    return 0;
}

/*==========================================
 * 状態異常にかかる(確率指定) 
 *------------------------------------------
 */
BUILDIN_FUNC(sc_start2)
{
    struct block_list *bl;
    int  type, tick, val1, per;
    type = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    tick = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    val1 = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    per = conv_num (st, &(st->stack->stack_data[st->start + 5]));
    if (st->end > st->start + 6)    //指定したキャラを状態異常にする 
        bl = map_id2bl (conv_num
                        (st, &(st->stack->stack_data[st->start + 6])));
    else
        bl = map_id2bl (st->rid);
    if (!bl)
        return 0;

    if (bl->type == BL_PC
        && ((struct map_session_data *) bl)->state.potionpitcher_flag)
        bl = map_id2bl (((struct map_session_data *) bl)->skilltarget);
    if (MRAND (10000) < per)
        skill_status_change_start (bl, type, val1, 0, 0, 0, tick, 0);
    return 0;
}

/*==========================================
 * 状態異常が直る 
 *------------------------------------------
 */
BUILDIN_FUNC(sc_end)
{
    struct block_list *bl;
    int  type;
    type = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    bl = map_id2bl (st->rid);
    if (!bl)
        return 0;

    if (bl->type == BL_PC
        && ((struct map_session_data *) bl)->state.potionpitcher_flag)
        bl = map_id2bl (((struct map_session_data *) bl)->skilltarget);
    skill_status_change_end (bl, type, -1);
//  if(battle_config.etc_log)
//      printf("sc_end : %d %d\n",st->rid,type);
    return 0;
}

BUILDIN_FUNC(sc_check)
{
    struct block_list *bl;
    int  type;
    type = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    bl = map_id2bl (st->rid);
    if (!bl)
    {
        push_val (st->stack, C_INT, 0);
        return 0;
    }

    if (bl->type == BL_PC
        && ((struct map_session_data *) bl)->state.potionpitcher_flag)
        bl = map_id2bl (((struct map_session_data *) bl)->skilltarget);

    push_val (st->stack, C_INT, skill_status_change_active (bl, type));

    return 0;
}

/*==========================================
 * 状態異常耐性を計算した確率を返す 
 *------------------------------------------
 */
BUILDIN_FUNC(getscrate)
{
    struct block_list *bl;
    int  sc_def = 100, sc_def_mdef2, sc_def_vit2, sc_def_int2, sc_def_luk2;
    int  type, rate, luk;

    type = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    rate = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    if (st->end > st->start + 4)    //指定したキャラの耐性を計算する 
        bl = map_id2bl (conv_num
                        (st, &(st->stack->stack_data[st->start + 6])));
    else
        bl = map_id2bl (st->rid);

    luk = battle_get_luk (bl);
    sc_def_mdef2 = 100 - (3 + battle_get_mdef (bl) + luk / 3);
    sc_def_vit2 = 100 - (3 + battle_get_vit (bl) + luk / 3);
    sc_def_int2 = 100 - (3 + battle_get_int (bl) + luk / 3);
    sc_def_luk2 = 100 - (3 + luk);

    if (type == SC_STONE || type == SC_FREEZE)
        sc_def = sc_def_mdef2;
    else if (type == SC_STAN || type == SC_POISON || type == SC_SILENCE)
        sc_def = sc_def_vit2;
    else if (type == SC_SLEEP || type == SC_CONFUSION || type == SC_BLIND)
        sc_def = sc_def_int2;
    else if (type == SC_CURSE)
        sc_def = sc_def_luk2;

    rate = rate * sc_def / 100;
    push_val (st->stack, C_INT, rate);

    return 0;

}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(debugmes)
{
    conv_str (st, &(st->stack->stack_data[st->start + 2]));
    printf ("script debug : %d %d : %s\n", st->rid, st->oid,
            st->stack->stack_data[st->start + 2].u.str);
    return 0;
}

/*==========================================
 * Added - AppleGirl For Advanced Classes, (Updated for Cleaner Script Purposes)
 *------------------------------------------
 */
BUILDIN_FUNC(resetlvl)
{
    struct map_session_data *sd;

    int  type = conv_num (st, &(st->stack->stack_data[st->start + 2]));

    sd = script_rid2sd (st);
    pc_resetlvl (sd, type);
    return 0;
}

/*==========================================
 * ステータスリセット 
 *------------------------------------------
 */
BUILDIN_FUNC(resetstatus)
{
    struct map_session_data *sd;
    sd = script_rid2sd (st);
    pc_resetstate (sd);
    return 0;
}

/*==========================================
 * スキルリセット 
 *------------------------------------------
 */
BUILDIN_FUNC(resetskill)
{
    struct map_session_data *sd;
    sd = script_rid2sd (st);
    pc_resetskill (sd);
    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
BUILDIN_FUNC(changebase)
{
    struct map_session_data *sd = NULL;
    int  vclass;

    if (st->end > st->start + 3)
        sd = map_id2sd (conv_num
                        (st, &(st->stack->stack_data[st->start + 3])));
    else
        sd = script_rid2sd (st);

    if (sd == NULL)
        return 0;

    vclass = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    if (vclass == 22 && !battle_config.wedding_modifydisplay)
        return 0;

//  if(vclass==22) {
//      pc_unequipitem(sd,sd->equip_index[9],0);    // 装備外 
//  }

    sd->view_class = vclass;

    return 0;
}

/*==========================================
 * 性別変換 
 *------------------------------------------
 */
BUILDIN_FUNC(changesex)
{
    struct map_session_data *sd = NULL;
    sd = script_rid2sd (st);
    if (!sd)
        return 1;

    if (sd->status.sex == 0)
    {
        sd->status.sex = 1;
        sd->sex = 1;
        if (sd->status.class == 20 || sd->status.class == 4021)
            sd->status.class -= 1;
    }
    else if (sd->status.sex == 1)
    {
        sd->status.sex = 0;
        sd->sex = 0;
        if (sd->status.class == 19 || sd->status.class == 4020)
            sd->status.class += 1;
    }
    chrif_char_ask_name (-1, sd->status.name, 5, 0, 0, 0, 0, 0, 0); // type: 5 - changesex
    chrif_save (sd);
    return 0;
}

/*==========================================
 * npcチャット作成 
 *------------------------------------------
 */
BUILDIN_FUNC(waitingroom)
{
    char *name, *ev = "";
    int  limit, trigger = 0, pub = 1;
    name = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    limit = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    if (limit == 0)
        pub = 3;

    if ((st->end > st->start + 5))
    {
        struct script_data *data = &(st->stack->stack_data[st->start + 5]);
        get_val (st, data);
        if (!data)
            return 0;

        if (data->type == C_INT)
        {
            // 新Athena仕様(旧Athena仕様と互換性あり) 
            ev = conv_str (st, &(st->stack->stack_data[st->start + 4]));
            trigger = conv_num (st, &(st->stack->stack_data[st->start + 5]));
        }
        else
        {
            // eathena仕様 
            trigger = conv_num (st, &(st->stack->stack_data[st->start + 4]));
            ev = conv_str (st, &(st->stack->stack_data[st->start + 5]));
        }
    }
    else
    {
        // 旧Athena仕様 
        if (st->end > st->start + 4)
            ev = conv_str (st, &(st->stack->stack_data[st->start + 4]));
    }
    chat_createnpcchat ((struct npc_data *) map_id2bl (st->oid),
                        limit, pub, trigger, name, strlen (name) + 1, ev);
    return 0;
}

/*==========================================
 * npcチャット削除 
 *------------------------------------------
 */
BUILDIN_FUNC(delwaitingroom)
{
    struct npc_data *nd;
    if (st->end > st->start + 2)
        nd = npc_name2id (conv_str
                          (st, &(st->stack->stack_data[st->start + 2])));
    else
        nd = (struct npc_data *) map_id2bl (st->oid);
    chat_deletenpcchat (nd);
    return 0;
}

/*==========================================
 * npcチャット全員蹴り出す 
 *------------------------------------------
 */
BUILDIN_FUNC(waitingroomkickall)
{
    struct npc_data *nd;
    struct chat_data *cd;

    if (st->end > st->start + 2)
        nd = npc_name2id (conv_str
                          (st, &(st->stack->stack_data[st->start + 2])));
    else
        nd = (struct npc_data *) map_id2bl (st->oid);

    if (nd == NULL
        || (cd = (struct chat_data *) map_id2bl (nd->chat_id)) == NULL)
        return 0;
    chat_npckickall (cd);
    return 0;
}

/*==========================================
 * npcチャットイベント有効化 
 *------------------------------------------
 */
BUILDIN_FUNC(enablewaitingroomevent)
{
    struct npc_data *nd;
    struct chat_data *cd;

    if (st->end > st->start + 2)
        nd = npc_name2id (conv_str
                          (st, &(st->stack->stack_data[st->start + 2])));
    else
        nd = (struct npc_data *) map_id2bl (st->oid);

    if (nd == NULL
        || (cd = (struct chat_data *) map_id2bl (nd->chat_id)) == NULL)
        return 0;
    chat_enableevent (cd);
    return 0;
}

/*==========================================
 * npcチャットイベント無効化 
 *------------------------------------------
 */
BUILDIN_FUNC(disablewaitingroomevent)
{
    struct npc_data *nd;
    struct chat_data *cd;

    if (st->end > st->start + 2)
        nd = npc_name2id (conv_str
                          (st, &(st->stack->stack_data[st->start + 2])));
    else
        nd = (struct npc_data *) map_id2bl (st->oid);

    if (nd == NULL
        || (cd = (struct chat_data *) map_id2bl (nd->chat_id)) == NULL)
        return 0;
    chat_disableevent (cd);
    return 0;
}

/*==========================================
 * npcチャット状態所得 
 *------------------------------------------
 */
BUILDIN_FUNC(getwaitingroomstate)
{
    struct npc_data *nd;
    struct chat_data *cd;
    int  val = 0, type;
    type = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    if (st->end > st->start + 3)
        nd = npc_name2id (conv_str
                          (st, &(st->stack->stack_data[st->start + 3])));
    else
        nd = (struct npc_data *) map_id2bl (st->oid);

    if (nd == NULL
        || (cd = (struct chat_data *) map_id2bl (nd->chat_id)) == NULL)
    {
        push_val (st->stack, C_INT, -1);
        return 0;
    }

    switch (type)
    {
        case 0:
            val = cd->users;
            break;
        case 1:
            val = cd->limit;
            break;
        case 2:
            val = cd->trigger & 0x7f;
            break;
        case 3:
            val = ((cd->trigger & 0x80) > 0);
            break;
        case 32:
            val = (cd->users >= cd->limit);
            break;
        case 33:
            val = (cd->users >= cd->trigger);
            break;

        case 4:
            push_str (st->stack, C_CONSTSTR, cd->title);
            return 0;
        case 5:
            push_str (st->stack, C_CONSTSTR, cd->pass);
            return 0;
        case 16:
            push_str (st->stack, C_CONSTSTR, cd->npc_event);
            return 0;
        default:
            break;
    }
    push_val (st->stack, C_INT, val);
    return 0;
}

/*==========================================
 * チャットメンバー(規定人数)ワープ 
 *------------------------------------------
 */
BUILDIN_FUNC(warpwaitingpc)
{
    int  x, y, i, n;
    char *str;
    struct npc_data *nd = (struct npc_data *) map_id2bl (st->oid);
    struct chat_data *cd;

    if (nd == NULL
        || (cd = (struct chat_data *) map_id2bl (nd->chat_id)) == NULL)
        return 0;

    n = cd->trigger & 0x7f;
    str = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    x = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    y = conv_num (st, &(st->stack->stack_data[st->start + 4]));

    if (st->end > st->start + 5)
        n = conv_num (st, &(st->stack->stack_data[st->start + 5]));

    for (i = 0; i < n; i++)
    {
        struct map_session_data *sd = cd->usersd[0];    // リスト先頭のPCを次々に。 

        mapreg_setreg (add_str ("$@warpwaitingpc") + (i << 24), sd->bl.id);

        if (strcmp (str, "Random") == 0)
            pc_randomwarp (sd, 3);
        else if (strcmp (str, "SavePoint") == 0)
        {
            if (map[sd->bl.m].flag.noteleport)  // テレポ禁止 
                return 0;

            pc_setpos (sd, sd->status.save_point.map,
                       sd->status.save_point.x, sd->status.save_point.y, 3);
        }
        else
            pc_setpos (sd, str, x, y, 0);
    }
    mapreg_setreg (add_str ("$@warpwaitingpcnum"), n);
    return 0;
}

/*==========================================
 * RIDのアタッチ 
 *------------------------------------------
 */
BUILDIN_FUNC(attachrid)
{
    st->rid = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    push_val (st->stack, C_INT, (map_id2sd (st->rid) != NULL));
    return 0;
}

/*==========================================
 * RIDのデタッチ 
 *------------------------------------------
 */
BUILDIN_FUNC(detachrid)
{
    st->rid = 0;
    return 0;
}

/*==========================================
 * 存在チェック 
 *------------------------------------------
 */
BUILDIN_FUNC(isloggedin)
{
    push_val (st->stack, C_INT,
              map_id2sd (conv_num
                         (st,
                          &(st->stack->stack_data[st->start + 2]))) != NULL);
    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
enum
{ MF_NOMEMO, MF_NOTELEPORT, MF_NOSAVE, MF_NOBRANCH, MF_NOPENALTY,
    MF_NOZENYPENALTY, MF_PVP, MF_PVP_NOPARTY, MF_PVP_NOGUILD, MF_GVG,
    MF_GVG_NOPARTY, MF_NOTRADE, MF_NOSKILL, MF_NOWARP, MF_NOPVP,
    MF_NOICEWALL,
    MF_SNOW, MF_FOG, MF_SAKURA, MF_LEAVES, MF_RAIN
};

BUILDIN_FUNC(setmapflagnosave)
{
    int  m, x, y;
    char *str, *str2;

    str = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    str2 = conv_str (st, &(st->stack->stack_data[st->start + 3]));
    x = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    y = conv_num (st, &(st->stack->stack_data[st->start + 5]));
    m = map_mapname2mapid (str);
    if (m >= 0)
    {
        map[m].flag.nosave = 1;
        memcpy (map[m].save.map, str2, 16);
        map[m].save.map[15] = 0;
        map[m].save.x = x;
        map[m].save.y = y;
    }

    return 0;
}

BUILDIN_FUNC(setmapflag)
{
    int  m, i;
    char *str;

    str = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    i = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    m = map_mapname2mapid (str);
    if (m >= 0)
    {
        switch (i)
        {
            case MF_NOMEMO:
                map[m].flag.nomemo = 1;
                break;
            case MF_NOTELEPORT:
                map[m].flag.noteleport = 1;
                break;
            case MF_NOBRANCH:
                map[m].flag.nobranch = 1;
                break;
            case MF_NOPENALTY:
                map[m].flag.nopenalty = 1;
                break;
            case MF_PVP_NOPARTY:
                map[m].flag.pvp_noparty = 1;
                break;
            case MF_PVP_NOGUILD:
                map[m].flag.pvp_noguild = 1;
                break;
            case MF_GVG_NOPARTY:
                map[m].flag.gvg_noparty = 1;
                break;
            case MF_NOZENYPENALTY:
                map[m].flag.nozenypenalty = 1;
                break;
            case MF_NOTRADE:
                map[m].flag.notrade = 1;
                break;
            case MF_NOSKILL:
                map[m].flag.noskill = 1;
                break;
            case MF_NOWARP:
                map[m].flag.nowarp = 1;
                break;
            case MF_NOPVP:
                map[m].flag.nopvp = 1;
                break;
            case MF_NOICEWALL: // [Valaris]
                map[m].flag.noicewall = 1;
                break;
            case MF_SNOW:      // [Valaris]
                map[m].flag.snow = 1;
                break;
            case MF_FOG:       // [Valaris]
                map[m].flag.fog = 1;
                break;
            case MF_SAKURA:    // [Valaris]
                map[m].flag.sakura = 1;
                break;
            case MF_LEAVES:    // [Valaris]
                map[m].flag.leaves = 1;
                break;
            case MF_RAIN:      // [Valaris]
                map[m].flag.rain = 1;
                break;
            default:
                break;
        }
    }

    return 0;
}

BUILDIN_FUNC(removemapflag)
{
    int  m, i;
    char *str;

    str = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    i = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    m = map_mapname2mapid (str);
    if (m >= 0)
    {
        switch (i)
        {
            case MF_NOMEMO:
                map[m].flag.nomemo = 0;
                break;
            case MF_NOTELEPORT:
                map[m].flag.noteleport = 0;
                break;
            case MF_NOSAVE:
                map[m].flag.nosave = 0;
                break;
            case MF_NOBRANCH:
                map[m].flag.nobranch = 0;
                break;
            case MF_NOPENALTY:
                map[m].flag.nopenalty = 0;
                break;
            case MF_PVP_NOPARTY:
                map[m].flag.pvp_noparty = 0;
                break;
            case MF_PVP_NOGUILD:
                map[m].flag.pvp_noguild = 0;
                break;
            case MF_GVG_NOPARTY:
                map[m].flag.gvg_noparty = 0;
                break;
            case MF_NOZENYPENALTY:
                map[m].flag.nozenypenalty = 0;
                break;
            case MF_NOSKILL:
                map[m].flag.noskill = 0;
                break;
            case MF_NOWARP:
                map[m].flag.nowarp = 0;
                break;
            case MF_NOPVP:
                map[m].flag.nopvp = 0;
                break;
            case MF_NOICEWALL: // [Valaris]
                map[m].flag.noicewall = 0;
                break;
            case MF_SNOW:      // [Valaris]
                map[m].flag.snow = 0;
                break;
            case MF_FOG:       // [Valaris]
                map[m].flag.fog = 0;
                break;
            case MF_SAKURA:    // [Valaris]
                map[m].flag.sakura = 0;
                break;
            case MF_LEAVES:    // [Valaris]
                map[m].flag.leaves = 0;
                break;
            case MF_RAIN:      // [Valaris]
                map[m].flag.rain = 0;
                break;
            default:
                break;
        }
    }

    return 0;
}

BUILDIN_FUNC(pvpon)
{
    int  m;
    char *str;
    struct map_session_data *pl_sd = NULL;

    str = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    m = map_mapname2mapid (str);
    if (m >= 0 && !map[m].flag.pvp && !map[m].flag.nopvp)
    {
        int  i;
        map[m].flag.pvp = 1;
        clif_send0199 (m, 1);

        if (battle_config.pk_mode)  // disable ranking functions if pk_mode is on [Valaris]
            return 0;

        for (i = 0; i < fd_max; i++)
        {                       //人数分ループ 
            if (session[i] && (pl_sd = session[i]->session_data)
                && pl_sd->state.auth)
            {
                if (m == pl_sd->bl.m && pl_sd->pvp_timer == -1)
                {
                    pl_sd->pvp_timer =
                        add_timer (gettick () + 200, pc_calc_pvprank_timer,
                                   pl_sd->bl.id, 0);
                    pl_sd->pvp_rank = 0;
                    pl_sd->pvp_lastusers = 0;
                    pl_sd->pvp_point = 5;
                }
            }
        }
    }

    return 0;
}

BUILDIN_FUNC(pvpoff)
{
    int  m;
    char *str;
    struct map_session_data *pl_sd = NULL;

    str = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    m = map_mapname2mapid (str);
    if (m >= 0 && map[m].flag.pvp && map[m].flag.nopvp)
    {
        map[m].flag.pvp = 0;
        clif_send0199 (m, 0);

        if (battle_config.pk_mode)  // disable ranking options if pk_mode is on [Valaris]
            return 0;

        int  i;
        for (i = 0; i < fd_max; i++)
        {                       //人数分ループ 
            if (session[i] && (pl_sd = session[i]->session_data)
                && pl_sd->state.auth)
            {
                if (m == pl_sd->bl.m)
                {
                    clif_pvpset (pl_sd, 0, 0, 2);
                    if (pl_sd->pvp_timer != -1)
                    {
                        delete_timer (pl_sd->pvp_timer,
                                      pc_calc_pvprank_timer);
                        pl_sd->pvp_timer = -1;
                    }
                }
            }
        }
    }

    return 0;
}

BUILDIN_FUNC(gvgon)
{
    int  m;
    char *str;

    str = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    m = map_mapname2mapid (str);
    if (m >= 0 && !map[m].flag.gvg)
    {
        map[m].flag.gvg = 1;
        clif_send0199 (m, 3);
    }

    return 0;
}

BUILDIN_FUNC(gvgoff)
{
    int  m;
    char *str;

    str = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    m = map_mapname2mapid (str);
    if (m >= 0 && map[m].flag.gvg)
    {
        map[m].flag.gvg = 0;
        clif_send0199 (m, 0);
    }

    return 0;
}

/*==========================================
 * NPCエモーション 
 *------------------------------------------
 */

BUILDIN_FUNC(emotion)
{
    int  type;
    type = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    if (type < 0 || type > 100)
        return 0;
    clif_emotion (map_id2bl (st->oid), type);
    return 0;
}

int buildin_maprespawnguildid_sub (struct block_list *bl, va_list ap)
{
    if (!ap || !bl)
        return 0;

    int  g_id = va_arg (ap, int);
    int  flag = va_arg (ap, int);
    struct map_session_data *sd = NULL;
    struct mob_data *md = NULL;

    if (bl->type == BL_PC)
        sd = (struct map_session_data *) bl;
    if (bl->type == BL_MOB)
        md = (struct mob_data *) bl;

    if (sd)
    {
        if ((sd->status.guild_id == g_id) && (flag & 1))
            pc_setpos (sd, sd->status.save_point.map, sd->status.save_point.x,
                       sd->status.save_point.y, 3);
        else if ((sd->status.guild_id != g_id) && (flag & 2))
            pc_setpos (sd, sd->status.save_point.map, sd->status.save_point.x,
                       sd->status.save_point.y, 3);
        else if (sd->status.guild_id == 0)  // Warp out players not in guild [Valaris]
            pc_setpos (sd, sd->status.save_point.map, sd->status.save_point.x, sd->status.save_point.y, 3); // end addition [Valaris]
    }
    if (md && flag & 4)
    {
        if (md->class < 1285 || md->class > 1288)
            mob_delete (md);
    }
    return 0;
}

BUILDIN_FUNC(maprespawnguildid)
{
    char *mapname = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    int  g_id = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    int  flag = conv_num (st, &(st->stack->stack_data[st->start + 4]));

    int  m = map_mapname2mapid (mapname);

    if (m)
        map_foreachinarea (buildin_maprespawnguildid_sub, m, 0, 0,
                           map[m].xs - 1, map[m].ys - 1, BL_NUL, g_id, flag);
    return 0;
}

BUILDIN_FUNCU(agitstart)
{
    if (agit_flag == 1)
        return 1;               // Agit already Start.
    agit_flag = 1;
    guild_agit_start ();
    return 0;
}

BUILDIN_FUNCU(agitend)
{
    if (agit_flag == 0)
        return 1;               // Agit already End.
    agit_flag = 0;
    guild_agit_end ();
    return 0;
}

/*==========================================
 * agitcheck 1;    // choice script
 * if(@agit_flag == 1) goto agit;
 * if(agitcheck(0) == 1) goto agit;
 *------------------------------------------
 */
BUILDIN_FUNC(agitcheck)
{
    struct map_session_data *sd;
    int  cond;

    sd = script_rid2sd (st);
    cond = conv_num (st, &(st->stack->stack_data[st->start + 2]));

    if (cond == 0)
    {
        if (agit_flag == 1)
            push_val (st->stack, C_INT, 1);
        if (agit_flag == 0)
            push_val (st->stack, C_INT, 0);
    }
    else
    {
        if (agit_flag == 1)
            pc_setreg (sd, add_str ("@agit_flag"), 1);
        if (agit_flag == 0)
            pc_setreg (sd, add_str ("@agit_flag"), 0);
    }
    return 0;
}

BUILDIN_FUNC(flagemblem)
{
    struct npc_data *nd;
    int  g_id = conv_num (st, &(st->stack->stack_data[st->start + 2]));

    if (g_id < 0)
        return 0;
//  printf("Script.c: [FlagEmblem] GuildID=%d, Emblem=%d.\n", g->guild_id, g->emblem_id);

    nd = (struct npc_data *) map_id2bl (st->oid);
    if (nd)
        nd->u.scr.guild_id = g_id;
    return 1;
}

BUILDIN_FUNC(getcastlename)
{
    char *mapname = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    struct guild_castle *gc;
    int  i;
    char *buf = NULL;
    for (i = 0; i < MAX_GUILDCASTLE; i++)
    {
        if ((gc = guild_castle_search (i)) != NULL)
        {
            if (strcmp (mapname, gc->map_name) == 0)
            {
                buf = (char *) aCalloc (24, sizeof (char));
                safestrncpy (buf, gc->castle_name, 24);
                break;
            }
        }
    }
    if (buf)
        push_str (st->stack, C_STR, buf);
    else
        push_str (st->stack, C_CONSTSTR, "");
    return 0;
}

BUILDIN_FUNC(getcastledata)
{
    char *mapname = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    int  index = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    char *event = NULL;
    struct guild_castle *gc;
    int  i, j;

    if (st->end > st->start + 4 && index == 0)
    {
        for (i = 0, j = -1; i < MAX_GUILDCASTLE; i++)
            if ((gc = guild_castle_search (i)) != NULL &&
                strcmp (mapname, gc->map_name) == 0)
                j = i;
        if (j >= 0)
        {
            event = conv_str (st, &(st->stack->stack_data[st->start + 4]));
            guild_addcastleinfoevent (j, 17, event);
        }
    }

    for (i = 0; i < MAX_GUILDCASTLE; i++)
    {
        if ((gc = guild_castle_search (i)) != NULL)
        {
            if (strcmp (mapname, gc->map_name) == 0)
            {
                switch (index)
                {
                    case 0:
                        for (j = 1; j < 26; j++)
                            guild_castledataload (gc->castle_id, j);
                        break;  // Initialize[AgitInit]
                    case 1:
                        push_val (st->stack, C_INT, gc->guild_id);
                        break;
                    case 2:
                        push_val (st->stack, C_INT, gc->economy);
                        break;
                    case 3:
                        push_val (st->stack, C_INT, gc->defense);
                        break;
                    case 4:
                        push_val (st->stack, C_INT, gc->triggerE);
                        break;
                    case 5:
                        push_val (st->stack, C_INT, gc->triggerD);
                        break;
                    case 6:
                        push_val (st->stack, C_INT, gc->nextTime);
                        break;
                    case 7:
                        push_val (st->stack, C_INT, gc->payTime);
                        break;
                    case 8:
                        push_val (st->stack, C_INT, gc->createTime);
                        break;
                    case 9:
                        push_val (st->stack, C_INT, gc->visibleC);
                        break;
                    case 10:
                        push_val (st->stack, C_INT, gc->visibleG0);
                        break;
                    case 11:
                        push_val (st->stack, C_INT, gc->visibleG1);
                        break;
                    case 12:
                        push_val (st->stack, C_INT, gc->visibleG2);
                        break;
                    case 13:
                        push_val (st->stack, C_INT, gc->visibleG3);
                        break;
                    case 14:
                        push_val (st->stack, C_INT, gc->visibleG4);
                        break;
                    case 15:
                        push_val (st->stack, C_INT, gc->visibleG5);
                        break;
                    case 16:
                        push_val (st->stack, C_INT, gc->visibleG6);
                        break;
                    case 17:
                        push_val (st->stack, C_INT, gc->visibleG7);
                        break;
                    case 18:
                        push_val (st->stack, C_INT, gc->Ghp0);
                        break;
                    case 19:
                        push_val (st->stack, C_INT, gc->Ghp1);
                        break;
                    case 20:
                        push_val (st->stack, C_INT, gc->Ghp2);
                        break;
                    case 21:
                        push_val (st->stack, C_INT, gc->Ghp3);
                        break;
                    case 22:
                        push_val (st->stack, C_INT, gc->Ghp4);
                        break;
                    case 23:
                        push_val (st->stack, C_INT, gc->Ghp5);
                        break;
                    case 24:
                        push_val (st->stack, C_INT, gc->Ghp6);
                        break;
                    case 25:
                        push_val (st->stack, C_INT, gc->Ghp7);
                        break;
                    default:
                        push_val (st->stack, C_INT, 0);
                        break;
                }
                return 0;
            }
        }
    }
    push_val (st->stack, C_INT, 0);
    return 0;
}

BUILDIN_FUNC(setcastledata)
{
    char *mapname = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    int  index = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    int  value = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    struct guild_castle *gc;
    int  i;

    for (i = 0; i < MAX_GUILDCASTLE; i++)
    {
        if ((gc = guild_castle_search (i)) != NULL)
        {
            if (strcmp (mapname, gc->map_name) == 0)
            {
                // Save Data byself First
                switch (index)
                {
                    case 1:
                        gc->guild_id = value;
                        break;
                    case 2:
                        gc->economy = value;
                        break;
                    case 3:
                        gc->defense = value;
                        break;
                    case 4:
                        gc->triggerE = value;
                        break;
                    case 5:
                        gc->triggerD = value;
                        break;
                    case 6:
                        gc->nextTime = value;
                        break;
                    case 7:
                        gc->payTime = value;
                        break;
                    case 8:
                        gc->createTime = value;
                        break;
                    case 9:
                        gc->visibleC = value;
                        break;
                    case 10:
                        gc->visibleG0 = value;
                        break;
                    case 11:
                        gc->visibleG1 = value;
                        break;
                    case 12:
                        gc->visibleG2 = value;
                        break;
                    case 13:
                        gc->visibleG3 = value;
                        break;
                    case 14:
                        gc->visibleG4 = value;
                        break;
                    case 15:
                        gc->visibleG5 = value;
                        break;
                    case 16:
                        gc->visibleG6 = value;
                        break;
                    case 17:
                        gc->visibleG7 = value;
                        break;
                    case 18:
                        gc->Ghp0 = value;
                        break;
                    case 19:
                        gc->Ghp1 = value;
                        break;
                    case 20:
                        gc->Ghp2 = value;
                        break;
                    case 21:
                        gc->Ghp3 = value;
                        break;
                    case 22:
                        gc->Ghp4 = value;
                        break;
                    case 23:
                        gc->Ghp5 = value;
                        break;
                    case 24:
                        gc->Ghp6 = value;
                        break;
                    case 25:
                        gc->Ghp7 = value;
                        break;
                    default:
                        return 0;
                }
                guild_castledatasave (gc->castle_id, index, value);
                return 0;
            }
        }
    }
    return 0;
}

/* =====================================================================
 * ギルド情報を要求する 
 * ---------------------------------------------------------------------
 */
BUILDIN_FUNC(requestguildinfo)
{
    int  guild_id = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    char *event = NULL;

    if (st->end > st->start + 3)
        event = conv_str (st, &(st->stack->stack_data[st->start + 3]));

    if (guild_id > 0)
        guild_npc_request_info (guild_id, event);
    return 0;
}

/* =====================================================================
 * カードの数を得る 
 * ---------------------------------------------------------------------
 */
BUILDIN_FUNC(getequipcardcnt)
{
    int  i, num;
    struct map_session_data *sd;
    int  c = 4;

    num = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    if (num < 1 || num > 10)
    {
        push_val (st->stack, C_INT, 0);
        return 0;
    }
    sd = script_rid2sd (st);
    if (!sd)
    {
        push_val (st->stack, C_INT, 0);
        return 0;
    }
    i = pc_checkequip (sd, equip[num - 1]);
    if (sd->status.inventory[i].card[0] == 0x00ff)
    {                           // 製造武器はカードなし 
        push_val (st->stack, C_INT, 0);
        return 0;
    }
    do
    {
        if ((sd->status.inventory[i].card[c - 1] > 4000) &&
            (sd->status.inventory[i].card[c - 1] < 5000))
        {

            push_val (st->stack, C_INT, (c));
            return 0;
        }
    }
    while (c--);
    push_val (st->stack, C_INT, 0);
    return 0;
}

/* ================================================================
 * カード取り外し成功 
 * ----------------------------------------------------------------
 */
BUILDIN_FUNC(successremovecards)
{
    int  i, num, cardflag = 0, flag;
    struct map_session_data *sd;
    struct item item_tmp;
    int  c = 4;

    num = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    if (num < 1 || num > 10)
        return 1;
    sd = script_rid2sd (st);
    if (!sd)
        return 1;
    i = pc_checkequip (sd, equip[num - 1]);
    if (sd->status.inventory[i].card[0] == 0x00ff)
    {
        return 0;
    }
    do
    {
        if ((sd->status.inventory[i].card[c - 1] > 4000) &&
            (sd->status.inventory[i].card[c - 1] < 5000))
        {

            cardflag = 1;
            item_tmp.id = 0, item_tmp.nameid =
                sd->status.inventory[i].card[c - 1];
            item_tmp.equip = 0, item_tmp.identify = 1, item_tmp.refine = 0;
            item_tmp.attribute = 0;
            item_tmp.card[0] = 0, item_tmp.card[1] = 0, item_tmp.card[2] =
                0, item_tmp.card[3] = 0;

            if ((flag = pc_additem (sd, &item_tmp, 1)))
            {
                clif_additem (sd, 0, 0, flag);
                map_addflooritem (&item_tmp, 1, sd->bl.m, sd->bl.x, sd->bl.y,
                                  NULL, NULL, NULL, 0);
            }
        }
    }
    while (c--);

    if (cardflag == 1)
    {
        flag = 0;
        item_tmp.id = 0, item_tmp.nameid = sd->status.inventory[i].nameid;
        item_tmp.equip = 0, item_tmp.identify = 1, item_tmp.refine =
            sd->status.inventory[i].refine;
        item_tmp.attribute = sd->status.inventory[i].attribute;
        item_tmp.card[0] = 0, item_tmp.card[1] = 0, item_tmp.card[2] =
            0, item_tmp.card[3] = 0;
        pc_delitem (sd, i, 1, 0);
        if ((flag = pc_additem (sd, &item_tmp, 1)))
          {                           // もてないならドロップ 
            clif_additem (sd, 0, 0, flag);
            map_addflooritem (&item_tmp, 1, sd->bl.m, sd->bl.x, sd->bl.y,
                              NULL, NULL, NULL, 0);
        }
        clif_misceffect (&sd->bl, 3);
        return 0;
    }
    return 0;
}

/* ================================================================
 * カード取り外し失敗 slot,type 
 * type=0: 両方損失、1:カード損失、2:武具損失、3:損失無し 
 * ----------------------------------------------------------------
 */
BUILDIN_FUNC(failedremovecards)
{
    int  i, num, cardflag = 0, flag, typefail;
    struct map_session_data *sd;
    struct item item_tmp;
    int  c = 4;

    num = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    if (num < 1 || num > 10)
        return 1;
    typefail = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    sd = script_rid2sd (st);
    if (!sd)
        return 1;

    i = pc_checkequip (sd, equip[num - 1]);
    if (sd->status.inventory[i].card[0] == 0x00ff)
    {
        return 0;
    }
    do
    {
        if ((sd->status.inventory[i].card[c - 1] > 4000) &&
            (sd->status.inventory[i].card[c - 1] < 5000))
        {

            cardflag = 1;

            if (typefail == 2)
            {                   // 武具のみ損失なら、カードは受け取らせる 
                item_tmp.id = 0, item_tmp.nameid =
                    sd->status.inventory[i].card[c - 1];
                item_tmp.equip = 0, item_tmp.identify = 1, item_tmp.refine =
                    0;
                item_tmp.attribute = 0;
                item_tmp.card[0] = 0, item_tmp.card[1] = 0, item_tmp.card[2] =
                    0, item_tmp.card[3] = 0;
                if ((flag = pc_additem (sd, &item_tmp, 1)))
                {
                    clif_additem (sd, 0, 0, flag);
                    map_addflooritem (&item_tmp, 1, sd->bl.m, sd->bl.x,
                                      sd->bl.y, NULL, NULL, NULL, 0);
                }
            }
        }
    }
    while (c--);

    if (cardflag == 1)
    {

        if (typefail == 0 || typefail == 2)
        {                       // 武具損失 
            pc_delitem (sd, i, 1, 0);
            clif_misceffect (&sd->bl, 2);
            return 0;
        }
        if (typefail == 1)
          {                             // カードのみ損失（武具を返す） 
            flag = 0;
            item_tmp.id = 0, item_tmp.nameid = sd->status.inventory[i].nameid;
            item_tmp.equip = 0, item_tmp.identify = 1, item_tmp.refine =
                sd->status.inventory[i].refine;
            item_tmp.attribute = sd->status.inventory[i].attribute;
            item_tmp.card[0] = 0, item_tmp.card[1] = 0, item_tmp.card[2] =
                0, item_tmp.card[3] = 0;
            pc_delitem (sd, i, 1, 0);
            if ((flag = pc_additem (sd, &item_tmp, 1)))
            {
                clif_additem (sd, 0, 0, flag);
                map_addflooritem (&item_tmp, 1, sd->bl.m, sd->bl.x, sd->bl.y,
                                  NULL, NULL, NULL, 0);
            }
        }
        clif_misceffect (&sd->bl, 2);
        return 0;
    }
    return 0;
}

BUILDIN_FUNC(mapwarp)   // Added by RoVeRT
{
    int  x, y, m;
    char *str;
    char *mapname;
    int  x0, y0, x1, y1;

    mapname = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    x0 = 0;
    y0 = 0;
    x1 = map[map_mapname2mapid (mapname)].xs;
    y1 = map[map_mapname2mapid (mapname)].ys;
    str = conv_str (st, &(st->stack->stack_data[st->start + 3]));
    x = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    y = conv_num (st, &(st->stack->stack_data[st->start + 5]));

    if ((m = map_mapname2mapid (mapname)) < 0)
        return 0;

    map_foreachinarea (buildin_areawarp_sub,
                       m, x0, y0, x1, y1, BL_PC, str, x, y);
    return 0;
}

BUILDIN_FUNC(cmdothernpc)   // Added by RoVeRT
{
    char *npc, *command;

    npc = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    command = conv_str (st, &(st->stack->stack_data[st->start + 3]));

    npc_command (map_id2sd (st->rid), npc, command);
    return 0;
}

BUILDIN_FUNC(inittimer) // Added by RoVeRT
{
//  struct npc_data *nd=(struct npc_data*)map_id2bl(st->oid);

//  nd->lastaction=nd->timer=gettick();
    npc_do_ontimer (st->oid, map_id2sd (st->rid), 1);

    return 0;
}

BUILDIN_FUNC(stoptimer) // Added by RoVeRT
{
//  struct npc_data *nd=(struct npc_data*)map_id2bl(st->oid);

//  nd->lastaction=nd->timer=-1;
    npc_do_ontimer (st->oid, map_id2sd (st->rid), 0);

    return 0;
}

int buildin_mobcount_sub (struct block_list *bl, va_list ap)    // Added by RoVeRT
{
    if (!ap || !bl)
        return 0;

    char *event = va_arg (ap, char *);
    if (!event)
        return 0;

    int *c = va_arg (ap, int *);
    if (!c)
        return 0;

    if (strcmp (event, ((struct mob_data *) bl)->npc_event) == 0)
        (*c)++;
    return 0;
}

BUILDIN_FUNC(mobcount)  // Added by RoVeRT
{
    char *mapname, *event;
    int  m, c = 0;
    mapname = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    event = conv_str (st, &(st->stack->stack_data[st->start + 3]));

    if ((m = map_mapname2mapid (mapname)) < 0)
    {
        push_val (st->stack, C_INT, -1);
        return 0;
    }
    map_foreachinarea (buildin_mobcount_sub,
                       m, 0, 0, map[m].xs, map[m].ys, BL_MOB, event, &c);

    push_val (st->stack, C_INT, (c - 1));

    return 0;
}

BUILDIN_FUNC(marriage)
{
    char *partner = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    struct map_session_data *sd = script_rid2sd (st);
    struct map_session_data *p_sd = map_nick2sd (partner);

    if (sd == NULL || p_sd == NULL || pc_marriage (sd, p_sd) < 0)
    {
        push_val (st->stack, C_INT, 0);
        return 0;
    }
    push_val (st->stack, C_INT, 1);
    return 0;
}

BUILDIN_FUNC(wedding_effect)
{
    struct map_session_data *sd = script_rid2sd (st);

    if (sd == NULL)
        return 0;
    clif_wedding_effect (&sd->bl);
    return 0;
}

BUILDIN_FUNC(divorce)
{
    struct map_session_data *sd = script_rid2sd (st);
    if (!sd)
        return 0;

    st->state = STOP;           // rely on pc_divorce to restart

    sd->npc_flags.divorce = 1;

    if (sd == NULL || pc_divorce (sd) < 0)
    {
        push_val (st->stack, C_INT, 0);
        return 0;
    }

    push_val (st->stack, C_INT, 1);
    return 0;
}

/*================================================
 * Script for Displaying MOB Information [Valaris]
 *------------------------------------------------
 */
BUILDIN_FUNC(strmobinfo)
{

    int  num = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    int  class = conv_num (st, &(st->stack->stack_data[st->start + 3]));

    if (num <= 0 || num >= 8 || (class >= 0 && class <= 1000) || class > 2000)
        return 0;

    if (num == 1)
    {
        char *buf;
        buf = mob_db[class].name;
        push_str (st->stack, C_STR, buf);
        return 0;
    }
    else if (num == 2)
    {
        char *buf;
        buf = mob_db[class].jname;
        push_str (st->stack, C_STR, buf);
        return 0;
    }
    else if (num == 3)
        push_val (st->stack, C_INT, mob_db[class].lv);
    else if (num == 4)
        push_val (st->stack, C_INT, mob_db[class].max_hp);
    else if (num == 5)
        push_val (st->stack, C_INT, mob_db[class].max_sp);
    else if (num == 6)
        push_val (st->stack, C_INT, mob_db[class].base_exp);
    else if (num == 7)
        push_val (st->stack, C_INT, mob_db[class].job_exp);
    return 0;
}

/*==========================================
 * Summon guardians [Valaris]
 *------------------------------------------
 */
BUILDIN_FUNC(guardian)
{
    int  class = 0, amount = 1, x = 0, y = 0, guardian = 0;
    char *str, *map, *event = "";

    map = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    x = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    y = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    str = conv_str (st, &(st->stack->stack_data[st->start + 5]));
    class = conv_num (st, &(st->stack->stack_data[st->start + 6]));
    amount = conv_num (st, &(st->stack->stack_data[st->start + 7]));
    event = conv_str (st, &(st->stack->stack_data[st->start + 8]));
    if (st->end > st->start + 9)
        guardian = conv_num (st, &(st->stack->stack_data[st->start + 9]));

    mob_spawn_guardian (map_id2sd (st->rid), map, x, y, str, class, amount,
                        event, guardian);

    return 0;
}

/*================================================
 * Script for Displaying Guardian Info [Valaris]
 *------------------------------------------------
 */
BUILDIN_FUNC(guardianinfo)
{
    int  guardian = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    struct map_session_data *sd = script_rid2sd (st);
    if (!sd)
    {
        push_val (st->stack, C_INT, -1);
        return 1;
    }
    struct guild_castle *gc = guild_mapname2gc (map[sd->bl.m].name);

    if (guardian == 0 && gc->visibleG0 == 1)
        push_val (st->stack, C_INT, gc->Ghp0);
    if (guardian == 1 && gc->visibleG1 == 1)
        push_val (st->stack, C_INT, gc->Ghp1);
    if (guardian == 2 && gc->visibleG2 == 1)
        push_val (st->stack, C_INT, gc->Ghp2);
    if (guardian == 3 && gc->visibleG3 == 1)
        push_val (st->stack, C_INT, gc->Ghp3);
    if (guardian == 4 && gc->visibleG4 == 1)
        push_val (st->stack, C_INT, gc->Ghp4);
    if (guardian == 5 && gc->visibleG5 == 1)
        push_val (st->stack, C_INT, gc->Ghp5);
    if (guardian == 6 && gc->visibleG6 == 1)
        push_val (st->stack, C_INT, gc->Ghp6);
    if (guardian == 7 && gc->visibleG7 == 1)
        push_val (st->stack, C_INT, gc->Ghp7);
    else
        push_val (st->stack, C_INT, -1);

    return 0;
}

/*==========================================
 * IDからItem名 
 *------------------------------------------
 */
BUILDIN_FUNC(getitemname)
{
    struct item_data *i_data;
    char *item_name;
    struct script_data *data;

    data = &(st->stack->stack_data[st->start + 2]);
    get_val (st, data);
    if (data->type == C_STR || data->type == C_CONSTSTR)
    {
        const char *name = conv_str (st, data);
        i_data = itemdb_searchname (name);
    }
    else
    {
        int  item_id = conv_num (st, data);
        i_data = itemdb_search (item_id);
    }

    item_name = (char *) aCalloc (24, sizeof (char));
    if (i_data)
        safestrncpy (item_name, i_data->jname, 24);
    else
        safestrncpy (item_name, "Unknown Item", 24);

    push_str (st->stack, C_STR, item_name);

    return 0;
}

BUILDIN_FUNC(getspellinvocation)
{
    char *name;
    char *invocation;

    name = conv_str (st, &(st->stack->stack_data[st->start + 2]));

    invocation = magic_find_invocation (name);
    if (!invocation)
        invocation = "...";

    push_str (st->stack, C_STR, strdup (invocation));
    return 0;
}

BUILDIN_FUNC(getanchorinvocation)
{
    char *name;
    char *invocation;

    name = conv_str (st, &(st->stack->stack_data[st->start + 2]));

    invocation = magic_find_anchor_invocation (name);
    if (!invocation)
        invocation = "...";

    push_str (st->stack, C_STR, strdup (invocation));
    return 0;
}

BUILDIN_FUNC(getpartnerid)
{
    struct map_session_data *sd = script_rid2sd (st);
    if (sd)
        push_val (st->stack, C_INT, sd->status.partner_id);
    else
        push_val (st->stack, C_INT, 0);
    return 0;
}

/*==========================================
 * PCの所持品情報読み取り 
 *------------------------------------------
 */
BUILDIN_FUNC(getinventorylist)
{
    struct map_session_data *sd = script_rid2sd (st);
    int  i, j = 0;
    if (!sd)
        return 0;
    for (i = 0; i < MAX_INVENTORY; i++)
    {
        if (sd->status.inventory[i].nameid > 0
            && sd->status.inventory[i].amount > 0)
        {
            pc_setreg (sd, add_str ("@inventorylist_id") + (j << 24),
                       sd->status.inventory[i].nameid);
            pc_setreg (sd, add_str ("@inventorylist_amount") + (j << 24),
                       sd->status.inventory[i].amount);
            pc_setreg (sd, add_str ("@inventorylist_equip") + (j << 24),
                       sd->status.inventory[i].equip);
            pc_setreg (sd, add_str ("@inventorylist_refine") + (j << 24),
                       sd->status.inventory[i].refine);
            pc_setreg (sd, add_str ("@inventorylist_identify") + (j << 24),
                       sd->status.inventory[i].identify);
            pc_setreg (sd, add_str ("@inventorylist_attribute") + (j << 24),
                       sd->status.inventory[i].attribute);
            pc_setreg (sd, add_str ("@inventorylist_card1") + (j << 24),
                       sd->status.inventory[i].card[0]);
            pc_setreg (sd, add_str ("@inventorylist_card2") + (j << 24),
                       sd->status.inventory[i].card[1]);
            pc_setreg (sd, add_str ("@inventorylist_card3") + (j << 24),
                       sd->status.inventory[i].card[2]);
            pc_setreg (sd, add_str ("@inventorylist_card4") + (j << 24),
                       sd->status.inventory[i].card[3]);
            j++;
        }
    }
    pc_setreg (sd, add_str ("@inventorylist_count"), j);
    return 0;
}

BUILDIN_FUNC(getskilllist)
{
    struct map_session_data *sd = script_rid2sd (st);
    int  i, j = 0;
    if (!sd)
        return 0;
    for (i = 0; i < MAX_SKILL; i++)
    {
        if (sd->status.skill[i].id > 0 && sd->status.skill[i].lv > 0)
        {
            pc_setreg (sd, add_str ("@skilllist_id") + (j << 24),
                       sd->status.skill[i].id);
            pc_setreg (sd, add_str ("@skilllist_lv") + (j << 24),
                       sd->status.skill[i].lv);
            pc_setreg (sd, add_str ("@skilllist_flag") + (j << 24),
                       sd->status.skill[i].flags);
            j++;
        }
    }
    pc_setreg (sd, add_str ("@skilllist_count"), j);
    return 0;
}

BUILDIN_FUNC(get_activated_pool_skills)
{
    struct map_session_data *sd = script_rid2sd (st);
    int  pool_skills[MAX_SKILL_POOL];
    int  skill_pool_size = skill_pool (sd, pool_skills);
    int  i, count = 0;

    if (!sd)
        return 0;

    for (i = 0; i < skill_pool_size; i++)
    {
        int  skill_id = pool_skills[i];

        if (sd->status.skill[skill_id].id == skill_id)
        {
            pc_setreg (sd, add_str ("@skilllist_id") + (count << 24),
                       sd->status.skill[skill_id].id);
            pc_setreg (sd, add_str ("@skilllist_lv") + (count << 24),
                       sd->status.skill[skill_id].lv);
            pc_setreg (sd, add_str ("@skilllist_flag") + (count << 24),
                       sd->status.skill[skill_id].flags);
            pc_setregstr (sd, add_str ("@skilllist_name$") + (count << 24),
                          skill_name (skill_id));
            ++count;
        }
    }
    pc_setreg (sd, add_str ("@skilllist_count"), count);

    return 0;
}

//extern int skill_pool_skills[];
//extern int skill_pool_skills_size;

BUILDIN_FUNC(get_unactivated_pool_skills)
{
    struct map_session_data *sd = script_rid2sd (st);
    int  i, count = 0;

    if (!sd)
        return 0;

    for (i = 0; i < skill_pool_skills_size; i++)
    {
        int  skill_id = skill_pool_skills[i];

        if (sd->status.skill[skill_id].id == skill_id && !(sd->status.skill[skill_id].flags & SKILL_POOL_ACTIVATED))
        {
            pc_setreg (sd, add_str ("@skilllist_id") + (count << 24),
                       sd->status.skill[skill_id].id);
            pc_setreg (sd, add_str ("@skilllist_lv") + (count << 24),
                       sd->status.skill[skill_id].lv);
            pc_setreg (sd, add_str ("@skilllist_flag") + (count << 24),
                       sd->status.skill[skill_id].flags);
            pc_setregstr (sd, add_str ("@skilllist_name$") + (count << 24),
                          skill_name (skill_id));
            ++count;
        }
    }
    pc_setreg (sd, add_str ("@skilllist_count"), count);

    return 0;
}

BUILDIN_FUNC(get_pool_skills)
{
    struct map_session_data *sd = script_rid2sd (st);
    int  i, count = 0;

    if (!sd)
        return 0;

    for (i = 0; i < skill_pool_skills_size; i++)
    {
        int  skill_id = skill_pool_skills[i];

        if (sd->status.skill[skill_id].id == skill_id)
        {
            pc_setreg (sd, add_str ("@skilllist_id") + (count << 24),
                       sd->status.skill[skill_id].id);
            pc_setreg (sd, add_str ("@skilllist_lv") + (count << 24),
                       sd->status.skill[skill_id].lv);
            pc_setreg (sd, add_str ("@skilllist_flag") + (count << 24),
                       sd->status.skill[skill_id].flags);
            pc_setregstr (sd, add_str ("@skilllist_name$") + (count << 24),
                          skill_name (skill_id));
            ++count;
        }
    }
    pc_setreg (sd, add_str ("@skilllist_count"), count);

    return 0;
}

BUILDIN_FUNC(activate_pool_skill)
{
    struct map_session_data *sd = script_rid2sd (st);
    int  skill_id = conv_num (st, &(st->stack->stack_data[st->start + 2]));

    skill_pool_activate (sd, skill_id);
    clif_skillinfoblock (sd);

    return 0;
}

BUILDIN_FUNC(deactivate_pool_skill)
{
    struct map_session_data *sd = script_rid2sd (st);
    int  skill_id = conv_num (st, &(st->stack->stack_data[st->start + 2]));

    skill_pool_deactivate (sd, skill_id);
    clif_skillinfoblock (sd);

    return 0;
}

BUILDIN_FUNC(check_pool_skill)
{
    struct map_session_data *sd = script_rid2sd (st);
    int  skill_id = conv_num (st, &(st->stack->stack_data[st->start + 2]));

    push_val (st->stack, C_INT, skill_pool_is_activated (sd, skill_id));

    return 0;
}

BUILDIN_FUNC(clearitem)
{
    struct map_session_data *sd = script_rid2sd (st);
    int  i;
    if (sd == NULL)
        return 0;
    for (i = 0; i < MAX_INVENTORY; i++)
    {
        if (sd->status.inventory[i].amount)
            pc_delitem (sd, i, sd->status.inventory[i].amount, 0);
    }
    return 0;
}

/*==========================================
 * NPCクラスチェンジ 
 * classは変わりたいclass 
 * typeは通常0なのかな？ 
 *------------------------------------------
 */
BUILDIN_FUNC(classchange)
{
    int  class, type;
    struct block_list *bl = map_id2bl (st->oid);

    if (bl == NULL)
        return 0;

    class = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    type = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    clif_class_change (bl, class, type);
    return 0;
}

/*==========================================
 * misceffect(effect, [target])
 *
 * effect The effect type/ID.
 * target The player name or being ID on
 *  which to display the effect. If not
 *  specified, it attempts to default to
 *  the current NPC or invoking PC.
 *------------------------------------------
 */
BUILDIN_FUNC(misceffect)
{
    int  type;
    int  id = 0;
    char *name = NULL;
    struct block_list *bl = NULL;

    type = conv_num (st, &(st->stack->stack_data[st->start + 2]));

    if (st->end > st->start + 3)
    {
        struct script_data *sdata = &(st->stack->stack_data[st->start + 3]);

//+++ need check sdata for NULL?

        get_val (st, sdata);

        if (sdata->type == C_STR || sdata->type == C_CONSTSTR)
            name = conv_str (st, sdata);
        else
            id = conv_num (st, sdata);
    }

    if (name)
    {
        struct map_session_data *sd = map_nick2sd (name);
        if (sd)
            bl = &sd->bl;
    }
    else if (id)
        bl = map_id2bl (id);
    else if (st->oid)
        bl = map_id2bl (st->oid);
    else
    {
        struct map_session_data *sd = script_rid2sd (st);
        if (sd)
            bl = &sd->bl;
    }

    if (bl)
        clif_misceffect (bl, type);

    return 0;
}

/*==========================================
 * サウンドエフェクト 
 *------------------------------------------
 */
BUILDIN_FUNC(soundeffect)
{
    struct map_session_data *sd = script_rid2sd (st);
    if (!sd)
        return 1;

    char *name;
    int  type = 0;

    name = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    type = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    if (sd)
    {
        if (st->oid)
            clif_soundeffect (sd, map_id2bl (st->oid), name, type);
        else
            clif_soundeffect (sd, &sd->bl, name, type);
    }
    return 0;
}

/*==========================================
 * NPC skill effects [Valaris]
 *------------------------------------------
 */
BUILDIN_FUNC(npcskilleffect)
{
    struct npc_data *nd = (struct npc_data *) map_id2bl (st->oid);
    if (!nd)
        return 1;

    int  skillid = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    int  skilllv = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    int  x = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    int  y = conv_num (st, &(st->stack->stack_data[st->start + 5]));

    clif_skill_poseffect (&nd->bl, skillid, skilllv, x, y, gettick ());

    return 0;
}

/*==========================================
 * Special effects [Valaris]
 *------------------------------------------
 */
BUILDIN_FUNC(specialeffect)
{
    struct block_list *bl = map_id2bl (st->oid);

    if (bl == NULL)
        return 0;

    clif_specialeffect (bl,
                        conv_num (st,
                                  &(st->stack->stack_data[st->start + 2])),
                        0);

    return 0;
}

BUILDIN_FUNC(specialeffect2)
{
    struct map_session_data *sd = script_rid2sd (st);

    if (sd == NULL)
        return 0;

    clif_specialeffect (&sd->bl,
                        conv_num (st,
                                  &(st->stack->stack_data[st->start + 2])),
                        0);

    return 0;
}

/*==========================================
 * Nude [Valaris]
 *------------------------------------------
 */

BUILDIN_FUNC(nude)
{
    struct map_session_data *sd = script_rid2sd (st);
    int  i;

    if (sd == NULL)
        return 0;

    for (i = 0; i < MAX_EQUIP_SIZE; i++)
        if (sd->equip_index[i] >= 0)
            pc_unequipitem (sd, sd->equip_index[i], i);
    pc_calcstatus (sd, 0);

    return 0;
}

/*==========================================
 * UnequipById [Freeyorp]
 *------------------------------------------
 */

BUILDIN_FUNC(unequip_by_id)
{
    struct map_session_data *sd = script_rid2sd (st);
    if (sd == NULL)
        return 0;

    int  slot_id = conv_num (st, &(st->stack->stack_data[st->start + 2]));

    if (slot_id >= 0 && slot_id < MAX_EQUIP_SIZE && sd->equip_index[slot_id] >= 0)
        pc_unequipitem (sd, sd->equip_index[slot_id], slot_id);

    pc_calcstatus (sd, 0);

    return 0;
}

/*==========================================
 * gmcommand [MouseJstr]
 *
 * suggested on the forums...
 *------------------------------------------
 */

BUILDIN_FUNC(gmcommand)
{
    struct map_session_data *sd;
    char *cmd;

    sd = script_rid2sd (st);
    cmd = conv_str (st, &(st->stack->stack_data[st->start + 2]));

    is_atcommand (sd->fd, sd, cmd, 99);

    return 0;
}

/*==========================================
 * movenpc [MouseJstr]
 *------------------------------------------
 */

BUILDIN_FUNCU(movenpc)
{
/*
    struct map_session_data *sd;
    char *map, *npc;
    int  x, y;

    sd = script_rid2sd (st);

    map = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    x = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    y = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    npc = conv_str (st, &(st->stack->stack_data[st->start + 5]));
*/

    return 0;
}

/*==========================================
 * npcwarp [remoitnane]
 * Move NPC to a new position on the same map.
 *------------------------------------------
 */
BUILDIN_FUNC(npcwarp)
{
    int  x, y;
    char *npc;
    struct npc_data *nd = NULL;

    x = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    y = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    npc = conv_str (st, &(st->stack->stack_data[st->start + 4]));
    nd = npc_name2id (npc);

    if (!nd)
        return -1;

    short m = nd->bl.m;

    /* Crude sanity checks. */
    if (m < 0 || !nd->bl.prev
            || x < 0 || x > map[m].xs -1
            || y < 0 || y > map[m].ys - 1)
        return -1;

    npc_enable (npc, 0);
    map_delblock(&nd->bl); /* [Freeyorp] */
    nd->bl.x = x;
    nd->bl.y = y;
    map_addblock(&nd->bl);
    npc_enable (npc, 1);

    return 0;
}

/*==========================================
 * message [MouseJstr]
 *------------------------------------------
 */

BUILDIN_FUNC(message)
{
    char *msg, *player;
    struct map_session_data *pl_sd = NULL;

    player = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    msg = conv_str (st, &(st->stack->stack_data[st->start + 3]));

    if ((pl_sd = map_nick2sd ((char *) player)) == NULL)
        return 1;
    clif_displaymessage (pl_sd->fd, msg);

    return 0;
}

/*==========================================
 * npctalk (sends message to surrounding
 * area) [Valaris]
 *------------------------------------------
 */

BUILDIN_FUNC(npctalk)
{
    char *str;
    char message[255];

    struct npc_data *nd = (struct npc_data *) map_id2bl (st->oid);
    str = conv_str (st, &(st->stack->stack_data[st->start + 2]));

    if (nd)
    {
        memcpy (message, nd->name, 24);
        strcat (message, " : ");
        strcat (message, str);
        clif_message (&(nd->bl), message);
    }

    return 0;
}

// change npc walkspeed [Valaris]
BUILDIN_FUNC(npcspeed)
{
    struct npc_data *nd = (struct npc_data *)map_id2bl(st->oid);
    int x = 0;

    x = script_getnum(st, 2);
    if(nd)
        nd->speed = x;

    return 0;
}

// make an npc walk to a position [Valaris]
BUILDIN_FUNCU(npcwalkto)
{
//    struct npc_data *nd = (struct npc_data *)map_id2bl(st->oid);
//    int x = 0, y = 0;

//    x = script_getnum(st, 2);
//    y = script_getnum(st, 3);
//    if(nd)
//        unit_walktoxy(&nd->bl, x, y,0);

    return 0;
}

// stop an npc's movement [Valaris]
BUILDIN_FUNCU(npcstop)
{
//    struct npc_data *nd = (struct npc_data *)map_id2bl(st->oid);

//    if(nd)
//        unit_stop_walking(&nd->bl, 1);

    return 0;
}

/*==========================================
 * hasitems (checks to see if player has any
 * items on them, if so will return a 1)
 * [Valaris]
 *------------------------------------------
 */

BUILDIN_FUNC(hasitems)
{
    int  i;
    struct map_session_data *sd;

    sd = script_rid2sd (st);

    if (!sd)
    {
        push_val (st->stack, C_INT, 0);
        return 1;
    }

    for (i = 0; i < MAX_INVENTORY; i++)
    {
        if (sd->status.inventory[i].amount)
        {
            push_val (st->stack, C_INT, 1);
            return 0;
        }
    }

    push_val (st->stack, C_INT, 0);

    return 0;
}

/*==========================================
  * getlook char info. getlook(arg)
  *------------------------------------------
  */
BUILDIN_FUNC(getlook)
{
    int  type, val;
    struct map_session_data *sd;
    sd = script_rid2sd (st);
    if (!sd)
    {
        push_val (st->stack, C_INT, -1);
        return 1;
    }

    type = conv_num (st, &(st->stack->stack_data[st->start + 2]));
    val = -1;
    switch (type)
    {
        case LOOK_HAIR:        //1
            val = sd->status.hair;
            break;
        case LOOK_WEAPON:      //2
            val = sd->status.weapon;
            break;
        case LOOK_HEAD_BOTTOM: //3
            val = sd->status.head_bottom;
            break;
        case LOOK_HEAD_TOP:    //4
            val = sd->status.head_top;
            break;
        case LOOK_HEAD_MID:    //5
            val = sd->status.head_mid;
            break;
        case LOOK_HAIR_COLOR:  //6
            val = sd->status.hair_color;
            break;
        case LOOK_CLOTHES_COLOR:   //7
            val = sd->status.clothes_color;
            break;
        case LOOK_SHIELD:      //8
            val = sd->status.shield;
            break;
        case LOOK_SHOES:       //9
            break;
        default:
            break;
    }

    push_val (st->stack, C_INT, val);
    return 0;
}

/*==========================================
  *     get char save point. argument: 0- map name, 1- x, 2- y
  *------------------------------------------
*/
BUILDIN_FUNC(getsavepoint)
{
    int  x = -1, y = -1, type;
    char *mapname;
    struct map_session_data *sd;

    sd = script_rid2sd (st);

    type = conv_num (st, &(st->stack->stack_data[st->start + 2]));

    if (sd)
    {
        x = sd->status.save_point.x;
        y = sd->status.save_point.y;
    }
    switch (type)
    {
        case 0:
            mapname = calloc (24, 1);
            if (sd)
                safestrncpy (mapname, sd->status.save_point.map, 24);
            else
                *mapname = 0;

            push_str (st->stack, C_STR, mapname);
            break;
        case 1:
            push_val (st->stack, C_INT, x);
            break;
        case 2:
            push_val (st->stack, C_INT, y);
            break;
        default:
            break;
    }
    return 0;
}

/*==========================================
  * Get position for  char/npc/pet/mob objects. Added by Lorky
  *
  *     int getMapXY(MapName$,MapX,MapY,type,[CharName$]);
  *             where type:
  *                     MapName$ - String variable for output map name
  *                     MapX     - Integer variable for output coord X
  *                     MapY     - Integer variable for output coord Y
  *                     type     - type of object
  *                                0 - Character coord
  *                                1 - NPC coord
  *                                2 - Pet coord
  *                                3 - Mob coord (not released)
  *                                4 - Homun coord
  *                     CharName$ - Name object. If miss or "this" the current object
  *
  *             Return:
  *                     0        - success
  *                     -1       - some error, MapName$,MapX,MapY contains unknown value.
  *------------------------------------------*/
BUILDIN_FUNC(getmapxy)
{
    struct block_list *bl = NULL;
    TBL_PC *sd = NULL;

    int num;
    char *name;
    char prefix;

    int x,y,type;
    char mapname[MAP_NAME_LENGTH];
    memset(mapname, 0, sizeof(mapname));

    if (!data_isreference(script_getdata(st, 2)))
    {
        ShowWarning("script: buildin_getmapxy: not mapname variable\n");
        script_pushint(st, -1);
        return 1;
    }
    if (!data_isreference(script_getdata(st, 3)))
    {
        ShowWarning("script: buildin_getmapxy: not mapx variable\n");
        script_pushint(st, -1);
        return 1;
    }
    if (!data_isreference(script_getdata(st, 4)))
    {
        ShowWarning("script: buildin_getmapxy: not mapy variable\n");
        script_pushint(st, -1);
        return 1;
    }

    // Possible needly check function parameters on C_STR,C_INT,C_INT
    type = script_getnum(st, 5);

    switch (type)
    {
        case 0: //Get Character Position
            if (script_hasdata(st, 6))
                sd = map_nick2sd(script_getstr(st, 6));
            else
                sd=script_rid2sd(st);

            if (sd)
                bl = &sd->bl;
            break;
        case 1: //Get NPC Position
            if (script_hasdata(st,6))
            {
                struct npc_data *nd;
                nd = npc_name2id(script_getstr(st, 6));
                if (nd)
                    bl = &nd->bl;
            }
            else //In case the origin is not an npc?
                bl = map_id2bl(st->oid);
            break;
        case 2: //Get Pet Position
//            if(script_hasdata(st,6))
//                sd=map_nick2sd(script_getstr(st,6));
//            else
//                sd=script_rid2sd(st);

//            if (sd && sd->pd)
//                bl = &sd->pd->bl;
            break;
        case 3: //Get Mob Position
            break; //Not supported?
        case 4: //Get Homun Position
//            if(script_hasdata(st,6))
//                sd=map_nick2sd(script_getstr(st,6));
//            else
//                sd=script_rid2sd(st);

//            if (sd && sd->hd)
//                bl = &sd->hd->bl;
            break;
    }
    if (!bl)
    { //No object found.
        script_pushint(st,-1);
        return 0;
    }

    x = bl->x;
    y = bl->y;
    memcpy(mapname, map[bl->m].name, MAP_NAME_LENGTH);

    //Set MapName$
    num = st->stack->stack_data[st->start+2].u.num;
    name=(char *)(str_buf+str_data[num & 0x00ffffff].str);
    prefix = *name;

    if (not_server_variable(prefix))
        sd = script_rid2sd(st);
    else
        sd = NULL;
    set_reg(sd, num, name, (void*)mapname);
//    set_reg(st, sd, num, name, (void*)mapname, script_getref(st, 2));

    //Set MapX
    num = st->stack->stack_data[st->start + 3].u.num;
    name = (char *)(str_buf + str_data[num & 0x00ffffff].str);
    prefix = *name;

    if (not_server_variable(prefix))
        sd = script_rid2sd(st);
    else
        sd = NULL;
    set_reg(sd, num, name, (void*)x);
//    set_reg(st, sd, num, name, (void*)x, script_getref(st, 3));

    //Set MapY
    num = st->stack->stack_data[st->start + 4].u.num;
    name = (char *)(str_buf + str_data[num & 0x00ffffff].str);
    prefix = *name;

    if(not_server_variable(prefix))
        sd = script_rid2sd(st);
    else
        sd = NULL;
    set_reg(sd, num, name, (void*)y);
//    set_reg(st, sd, num, name, (void*)y, script_getref(st, 4));

    //Return Success value
    script_pushint(st, 0);
    return 0;
}

/*==========================================
 *     areatimer
 *------------------------------------------
 */
int buildin_areatimer_sub (struct block_list *bl, va_list ap)
{
    if (!ap)
        return 1;

    int  tick;
    char *event;
    tick = va_arg (ap, int);
    event = va_arg (ap, char *);
    pc_addeventtimer ((struct map_session_data *) bl, tick, event);
    return 0;
}

BUILDIN_FUNC(areatimer)
{
    int  tick, m;
    char *event;
    char *mapname;
    int  x0, y0, x1, y1;

    mapname = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    x0 = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    y0 = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    x1 = conv_num (st, &(st->stack->stack_data[st->start + 5]));
    y1 = conv_num (st, &(st->stack->stack_data[st->start + 6]));
    tick = conv_num (st, &(st->stack->stack_data[st->start + 7]));
    event = conv_str (st, &(st->stack->stack_data[st->start + 8]));

    if ((m = map_mapname2mapid (mapname)) < 0)
        return 0;

    map_foreachinarea (buildin_areatimer_sub,
                       m, x0, y0, x1, y1, BL_PC, tick, event);
    return 0;
}

/*==========================================
 * Check whether the PC is in the specified rectangle
 *------------------------------------------
 */
BUILDIN_FUNC(isin)
{
    int  x1, y1, x2, y2;
    char *str;
    struct map_session_data *sd = script_rid2sd (st);

    str = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    x1 = conv_num (st, &(st->stack->stack_data[st->start + 3]));
    y1 = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    x2 = conv_num (st, &(st->stack->stack_data[st->start + 5]));
    y2 = conv_num (st, &(st->stack->stack_data[st->start + 6]));

    if (!sd)
    {
        push_val (st->stack, C_INT, 0);
        return 1;
    }

    push_val (st->stack, C_INT,
              (sd->bl.x >= x1 && sd->bl.x <= x2)
              && (sd->bl.y >= y1 && sd->bl.y <= y2)
              && (!strcmp (str, map[sd->bl.m].name)));

    return 0;
}

// Trigger the shop on a (hopefully) nearby shop NPC
BUILDIN_FUNC(shop)
{
    struct map_session_data *sd = script_rid2sd (st);
    struct npc_data *nd;

    if (!sd)
        return 1;

    nd = npc_name2id (conv_str (st, &(st->stack->stack_data[st->start + 2])));
    if (!nd)
        return 1;

    buildin_close (st);
    clif_npcbuysell (sd, nd->bl.id);
    return 0;
}

/*==========================================
 * Check whether the PC is dead
 *------------------------------------------
 */
BUILDIN_FUNC(isdead)
{
    struct map_session_data *sd = script_rid2sd (st);

    push_val (st->stack, C_INT, pc_isdead (sd));
    return 0;
}

/*========================================
 * Changes a NPC name, and sprite
 *----------------------------------------
 */
BUILDIN_FUNC(fakenpcname)
{
    char *name, *newname;
    int  newsprite;
    struct npc_data *nd;

    name = conv_str (st, &(st->stack->stack_data[st->start + 2]));
    newname = conv_str (st, &(st->stack->stack_data[st->start + 3]));
    newsprite = conv_num (st, &(st->stack->stack_data[st->start + 4]));
    nd = npc_name2id (name);
    if (!nd)
        return 1;
    safestrncpy (nd->name, newname, sizeof(nd->name));
//    nd->name[sizeof(nd->name)-1] = '\0';
    nd->class = newsprite;

    // Refresh this npc
    npc_enable (name, 0);
    npc_enable (name, 1);

    return 0;
}

BUILDIN_FUNC(setcollision)
{
    const char *map;
    int x1, y1, x2, y2, m, type;

    map = script_getstr(st, 2);

    if( (m = map_mapname2mapid(map)) < 0 )
        return 0;

    x1 = script_getnum(st, 3);
    y1 = script_getnum(st, 4);
    x2 = script_getnum(st, 5);
    y2 = script_getnum(st, 6);
    type = script_getnum(st, 7);

    map_setcells(m, x1, y1, x2, y2, type);
    return 0;
}

BUILDIN_FUNC(spell)
{
    struct map_session_data *sd;
    char *message;
    char *spell;
    char *nick;

    if (!script_hasdata(st, 2))
        return 0;

    if (script_hasdata(st, 3))
    {
        nick = script_getstr(st, 2);
        sd = map_nick2sd(nick);
        message = script_getstr(st, 3);
    }
    else
    {
        nick = sd->status.name;
        sd = script_rid2sd (st);
        message = script_getstr(st, 2);
    }
    if (!sd)
        return 0;

    spell = aCalloc (strlen(nick) + strlen(message) + 10, 1);
    strcpy (spell, nick);
    strcat (spell, " : ");
    strcat (spell, message);

    magic_message (sd, spell, strlen(spell), 0);
    free (spell);
    return 0;
}

BUILDIN_FUNC(npcclick)
{
    struct map_session_data *sd;
    sd = script_rid2sd (st);
    struct npc_data *npc = npc_name2id(script_getstr(st, 2));

    if (!npc || !sd || sd->npc_id != 0)
    {
        script_pushint(st, 0);
        return 1;
    }
    npc_click (sd, npc->bl.id, 1);

    script_pushint(st, npc->bl.id);
    return 0;
}

BUILDIN_FUNC(npcattach)
{
    struct map_session_data *sd;
    sd = script_rid2sd (st);
    struct npc_data *nd = npc_name2id(script_getstr(st, 2));

    if (!nd || !sd || sd->npc_id != 0)
    {
        script_pushint(st, 0);
        return 1;
    }
    sd->npc_id = nd->bl.id;
    sd->npc_pos = run_script (nd->u.scr.script, 0, sd->bl.id, nd->bl.id);
    sd->npc_isservice = 1;

    script_pushint(st, nd->bl.id);
    return 0;
}

/*==========================================
 * Displays a message for the player only (like system messages like "you got an apple" )
 *------------------------------------------*/
BUILDIN_FUNC(dispbottom)
{
    TBL_PC *sd = script_rid2sd(st);
    const char *message;
    if (!sd)
        return 0;

    message = script_getstr(st, 2);
    if (!message)
        return 0;

    clif_displaymessage(sd->fd, message);
    return 0;
}

BUILDIN_FUNC(recovery)
{
    TBL_PC *sd;

    if (!script_hasdata(st, 2))
        sd = script_rid2sd(st);
    else
        sd = map_nick2sd (script_getstr(st, 2));
    if (!sd)
        return 0;

    if (pc_isdead(sd))
    {
        sd->status.hp = sd->status.max_hp;
        sd->status.sp = sd->status.max_sp;
        pc_setstand (sd);
        if (battle_config.pc_invincible_time > 0)
            pc_setinvincibletimer (sd, battle_config.pc_invincible_time);
        clif_updatestatus (sd, SP_HP);
        clif_updatestatus (sd, SP_SP);
        clif_resurrection (&sd->bl, 1);
    }
    else
    {
        pc_heal (sd, sd->status.max_hp - sd->status.hp, sd->status.max_sp - sd->status.sp);
    }
    return 0;
}

BUILDIN_FUNC(getmapmobs)
{
    char *mapname;
    int  m, bx, by;
    mapname = script_getstr(st, 2);
    int count = 0, c, i;
    struct block_list *bl;

    if ((m = map_mapname2mapid (mapname)) < 0)
    {
        push_val (st->stack, C_INT, -1);
        return 0;
    }

    for (by = 0; by <= (map[m].ys - 1) / BLOCK_SIZE; by ++)
    {
        const int by1 = by * map[m].bxs;
        for (bx = 0; bx <= (map[m].xs - 1) / BLOCK_SIZE; bx ++)
        {
            const int b2 = bx + by1;
            bl = map[m].block_mob[b2];
            c = map[m].block_mob_count[b2];
            for (i = 0; i < c && bl; i++, bl = bl->next)
            {
                if (bl)
                    count++;
            }
        }
    }

    push_val (st->stack, C_INT, count);

    return 0;
}

//=======================================================
// strlen [Valaris]
//-------------------------------------------------------
BUILDIN_FUNC(getstrlen)
{
    const char *str = script_getstr(st, 2);
    int len = (str) ? (int)strlen(str) : 0;

    script_pushint(st, len);
    return 0;
}

//=======================================================
// isalpha [Valaris]
//-------------------------------------------------------
BUILDIN_FUNC(charisalpha)
{
    const char *str = script_getstr(st, 2);
    int pos = script_getnum(st, 3);

    if (!str || pos < 0 || (unsigned int)pos >= strlen(str) || !ISALPHA(str[pos]))
    {
        script_pushint(st, 0);
        return 0;
    }

    script_pushint(st, 1);
    return 0;
}

// case-insensitive substring search [lordalfa]
BUILDIN_FUNC(compare)
{
    const char *message;
    const char *cmpstring;
    message = script_getstr(st, 2);
    cmpstring = script_getstr(st, 3);
    if (!message || !cmpstring)
    {
        script_pushint(st, 0);
        return 0;
    }
    script_pushint(st, (stristr(message, cmpstring) != NULL));
    return 0;
}

/*==========================================
 * Returns some values of an item [Lupus]
 * Price, Weight, etc...
    getiteminfo(itemID,n), where n
        0 value_buy;
        1 value_sell;
        2 type;
        3 sex;
        4 equip;
        5 weight;
        6 atk;
        7 def;
        8 range;
        9 magic_bonus
       10 slot;
       11 look;
       12 elv;
       13 wlv;
       14 view id
 *------------------------------------------*/
BUILDIN_FUNC(getiteminfo)
{
    int item_id, n;
    int *item_arr;
    struct item_data *i_data;

    item_id = script_getnum(st, 2);
    n = script_getnum(st, 3);
    i_data = itemdb_exists(item_id);

    if (i_data && n >= 0 && n < 14)
    {
        item_arr = (int*)&i_data->value_buy;
        script_pushint(st, item_arr[n]);
    }
    else
    {
        script_pushint(st, -1);
    }
    return 0;
}

/*==========================================
 * Set some values of an item [Lupus]
 * Price, Weight, etc...
    getiteminfo(itemID,n), where n
        0 value_buy;
        1 value_sell;
        2 type;
        3 sex;
        4 equip;
        5 weight;
        6 atk;
        7 def;
        8 range;
        9 magic_bonus
       10 slot;
       11 look;
       12 elv;
       13 wlv;
       14 view id
  * Returns Value or -1 if the wrong field's been set
 *------------------------------------------*/
BUILDIN_FUNC(setiteminfo)
{
    int item_id, n, value;
    int *item_arr;
    struct item_data *i_data;

    item_id = script_getnum(st, 2);
    n = script_getnum(st, 3);
    value = script_getnum(st, 4);
    i_data = itemdb_exists(item_id);

    if (i_data && n >= 0 && n < 14)
    {
        item_arr = (int*)&i_data->value_buy;
        item_arr[n] = value;
        script_pushint(st, value);
    }
    else
    {
        script_pushint(st, -1);
    }
    return 0;
}

// [zBuffer] List of mathematics commands --->
BUILDIN_FUNC(sqrt)
{
    double i, a;
    i = script_getnum(st, 2);
    a = sqrt(i);
    script_pushint(st, (int)a);
    return 0;
}

BUILDIN_FUNC(distance)
{
    int dx, dy;

    dx = script_getnum(st, 2) - script_getnum(st, 4);
    dy = script_getnum(st, 3) - script_getnum(st, 5);

    if (dx < 0)
        dx = -dx;
    if (dy < 0)
        dy = -dy;

    script_pushint(st, dx < dy ? dy : dx);
    return 0;
}
// <--- [zBuffer] List of mathematics commands
// [zBuffer] List of dynamic var commands --->
//FIXME: some other functions are using this private function
void setd_sub(TBL_PC *sd, char *varname, int elem, void *value)
{
    set_reg(sd, add_str(varname)+(elem<<24), varname, value);
    return;
}

BUILDIN_FUNC(setd)
{
    TBL_PC *sd = NULL;
    char varname[100];
    const char *value, *buffer;
    int elem;
    buffer = script_getstr(st, 2);
    value = script_getstr(st, 3);

    if(sscanf(buffer, "%99[^[][%d]", varname, &elem) < 2)
        elem = 0;

    if (not_server_variable(*varname))
    {
        sd = script_rid2sd(st);
        if (sd == NULL)
        {
            ShowError("script:setd: no player attached for player variable '%s'\n", buffer);
            return 0;
        }
    }

    if(varname[strlen(varname) - 1] != '$')
        setd_sub(sd, varname, elem, (void *)atoi(value));
    else
        setd_sub(sd, varname, elem, (void *)value);

    return 0;
}

BUILDIN_FUNC(getd)
{
    char varname[100];
    const char *buffer;
    int elem;

    buffer = script_getstr(st, 2);

    if(sscanf(buffer, "%99[^[][%d]", varname, &elem) < 2)
        elem = 0;

    // Push the 'pointer' so it's more flexible [Lance]
    push_val(st->stack, C_NAME, (elem<<24) | add_str(varname));

    return 0;
}
// <--- [zBuffer] List of dynamic var commands

BUILDIN_FUNC(callshop)
{
    TBL_PC *sd = NULL;
    struct npc_data *nd;
    const char *shopname;
    int flag = 0;
    sd = script_rid2sd(st);
    if (!sd)
    {
        script_pushint(st, 0);
        return 0;
    }
    shopname = script_getstr(st, 2);
    if (script_hasdata(st, 3))
        flag = script_getnum(st, 3);
    nd = npc_name2id(shopname);
    if (!nd || nd->bl.type != BL_NPC || nd->bl.subtype != SHOP)
    {
        ShowError("buildin_callshop: Shop [%s] not found (or NPC is not shop type)\n", shopname);
        script_pushint(st, 0);
        return 1;
    }

    if (nd->bl.subtype == SHOP)
    {
        switch (flag)
        {
            case 1:
                npc_buysellsel(sd, nd->bl.id, 0);
                break; //Buy window
            case 2:
                npc_buysellsel(sd, nd->bl.id,1);
                break; //Sell window
            default:
                clif_npcbuysell(sd, nd->bl.id);
                break; //Show menu
        }
        sd->npc_shopid = nd->bl.id;
        script_pushint(st, 1);
    }
    else
    {
        script_pushint(st, 0);
    }

    return 0;
}

BUILDIN_FUNC(npcshopitem)
{
    const char* npcname = script_getstr(st, 2);
    if (!npcname)
        return 1;

    struct npc_data* nd = npc_name2id(npcname);
    int n, i;
    int amount;

    if (!nd || nd->bl.subtype != SHOP || !nd->u.shop.shop_item)
    {   //Not found.
        script_pushint(st, 0);
        return 0;
    }

    // get the count of new entries
    amount = (script_lastdata(st) - 2) / 2;

    // generate new shop item list
    RECREATE(nd->u.shop.shop_item, struct npc_item_list, amount + 1);
    for (n = 0, i = 3; n < amount; n ++, i += 2)
    {
        nd->u.shop.shop_item[n].nameid = script_getnum(st, i);
        nd->u.shop.shop_item[n].value = script_getnum(st, i + 1);
    }
    nd->u.shop.shop_item[n].nameid = 0;
    nd->u.shop.shop_item[n].value = 0;
    nd->u.shop.count = n;

    script_pushint(st, 1);
    return 0;
}

BUILDIN_FUNC(npcshopadditem)
{
    const char* npcname = script_getstr(st, 2);
    if (!npcname)
        return 1;

    struct npc_data* nd = npc_name2id(npcname);
    int n, i;
    int amount;

    if (!nd || nd->bl.subtype != SHOP || !nd->u.shop.shop_item)
    {   //Not found.
        script_pushint(st, 0);
        return 0;
    }

    // get the count of new entries
    amount = (script_lastdata(st) - 2) / 2;

    // append new items to existing shop item list
    RECREATE(nd->u.shop.shop_item, struct npc_item_list, nd->u.shop.count + amount + 1);
    for (n = nd->u.shop.count, i = 3; n < nd->u.shop.count + amount; n++, i += 2)
    {
        nd->u.shop.shop_item[n].nameid = script_getnum(st, i);
        nd->u.shop.shop_item[n].value = script_getnum(st, i + 1);
    }
    nd->u.shop.shop_item[n].nameid = 0;
    nd->u.shop.shop_item[n].value = 0;
    nd->u.shop.count = n;

    script_pushint(st, 1);
    return 0;
}

BUILDIN_FUNC(npcshopdelitem)
{
    const char* npcname = script_getstr(st, 2);
    if (!npcname)
        return 1;

    struct npc_data* nd = npc_name2id(npcname);
    int n, i;
    int amount;
    int size;

    if (!nd || nd->bl.subtype != SHOP || !nd->u.shop.shop_item)
    {   //Not found.
        script_pushint(st, 0);
        return 0;
    }

    amount = script_lastdata(st) - 2;
    if (amount <= 0)
    {
        script_pushint(st, 0);
        return 0;
    }

    size = nd->u.shop.count;

    // remove specified items from the shop item list
    for (i = 3; i < 3 + amount; i ++)
    {
        ARR_FIND (0, size, n, nd->u.shop.shop_item[n].nameid == script_getnum(st,i));
        if (n < size)
        {
            memmove(&nd->u.shop.shop_item[n], &nd->u.shop.shop_item[n + 1], sizeof(nd->u.shop.shop_item[0])*(size - n));
            size--;
        }
    }

    RECREATE(nd->u.shop.shop_item, struct npc_item_list, size);
    nd->u.shop.count = size;

    script_pushint(st, 1);
    return 0;
}

BUILDIN_FUNC(equip)
{
    int nameid = 0, i;
    TBL_PC *sd;
    struct item_data *item_data;

    sd = script_rid2sd(st);
    if (!sd)
        return 0;

    nameid = script_getnum(st, 2);
    if((item_data = itemdb_exists(nameid)) == NULL)
    {
        ShowError("wrong item ID : equipitem(%i)\n", nameid);
        return 1;
    }
    ARR_FIND (0, MAX_INVENTORY, i, sd->status.inventory[i].nameid == nameid);
    if (i < MAX_INVENTORY)
        pc_equipitem(sd, i, item_data->equip);

    return 0;
}

/*==========================================
 * Returns some values of an item [Lupus]
 * Price, Weight, etc...
    setitemscript(itemID,"{new item bonus script}",[n]);
   Where n:
    0 - script
    1 - Equip script
    2 - Unequip script
 *------------------------------------------*/
BUILDIN_FUNC(setitemscript)
{
    int item_id, n = 0;
    char *script;
    struct item_data *i_data;
    char **dstscript;

    item_id = script_getnum(st, 2);
    script = script_getstr(st, 3);
    if (script_hasdata(st, 4))
        n = script_getnum(st, 4);
    i_data = itemdb_exists(item_id);

    if (!i_data || script == NULL || script[0] != '{')
    {
        script_pushint(st, 0);
        return 0;
    }
    switch (n)
    {
    case 2:
        dstscript = &i_data->unequip_script;
        break;
    case 1:
        dstscript = &i_data->equip_script;
        break;
    default:
        dstscript = &i_data->use_script;
        break;
    }
    if (!dstscript)
    {
        script_pushint(st, 0);
        return 0;
    }

    free (*dstscript);

    *dstscript = parse_script(script, 0);
    script_pushint(st, 1);
    return 0;
}

BUILDIN_FUNC(getmonsterinfo)
{
    struct mob_db *mob;
    int mob_id;

    mob_id = script_getnum(st, 2);
    if (!mobdb_checkid(mob_id))
    {
        ShowError("buildin_getmonsterinfo: Wrong Monster ID: %i\n", mob_id);
        if (!script_getnum(st, 3)) //requested a string
            script_pushconststr(st, "null");
        else
            script_pushint(st, -1);
        return -1;
    }

    mob = &mob_db[mob_id];
    switch (script_getnum(st, 3))
    {
        case 0:  script_pushstrcopy(st, mob->jname); break;
        case 1:  script_pushint(st, mob->lv); break;
        case 2:  script_pushint(st, mob->max_hp); break;
        case 3:  script_pushint(st, mob->base_exp); break;
        case 4:  script_pushint(st, mob->job_exp); break;
        case 5:  script_pushint(st, mob->atk1); break;
        case 6:  script_pushint(st, mob->atk2); break;
        case 7:  script_pushint(st, mob->def); break;
        case 8:  script_pushint(st, mob->mdef); break;
        case 9:  script_pushint(st, mob->str); break;
        case 10: script_pushint(st, mob->agi); break;
        case 11: script_pushint(st, mob->vit); break;
        case 12: script_pushint(st, mob->int_); break;
        case 13: script_pushint(st, mob->dex); break;
        case 14: script_pushint(st, mob->luk); break;
        case 15: script_pushint(st, mob->range); break;
        case 16: script_pushint(st, mob->range2); break;
        case 17: script_pushint(st, mob->range3); break;
        case 18: script_pushint(st, mob->size); break;
        case 19: script_pushint(st, mob->race); break;
        case 20: script_pushint(st, mob->element); break;
        case 21: script_pushint(st, mob->mode); break;
        default: script_pushint(st, -1); //wrong Index
    }
    return 0;
}

int axtoi(const char *hexStg)
{
    int n = 0;         // position in string
    int m = 0;         // position in digit[] to shift
    int count;         // loop index
    int intValue = 0;  // integer value of hex string
    int digit[11];      // hold values to convert
    while (n < 10)
    {
        if (hexStg[n]=='\0')
            break;
        if (hexStg[n] > 0x29 && hexStg[n] < 0x40 ) //if 0 to 9
            digit[n] = hexStg[n] & 0x0f;            //convert to int
        else if (hexStg[n] >='a' && hexStg[n] <= 'f') //if a to f
            digit[n] = (hexStg[n] & 0x0f) + 9;      //convert to int
        else if (hexStg[n] >='A' && hexStg[n] <= 'F') //if A to F
            digit[n] = (hexStg[n] & 0x0f) + 9;      //convert to int
        else
            break;
        n++;
    }
    count = n;
    m = n - 1;
    n = 0;
    while(n < count)
    {
        // digit[n] is value of hex digit at position n
        // (m << 2) is the number of positions to shift
        // OR the bits into return value
        intValue = intValue | (digit[n] << (m << 2));
        m--;   // adjust the position to set
        n++;   // next digit to process
    }
    return (intValue);
}

// [Lance] Hex string to integer converter
BUILDIN_FUNC(axtoi)
{
    const char *hex = script_getstr(st, 2);
    script_pushint(st,axtoi(hex));
    return 0;
}

BUILDIN_FUNC(atoi)
{
    const char *value;
    value = script_getstr(st, 2);
    script_pushint(st, atoi(value));
    return 0;
}

BUILDIN_FUNC(rid2name)
{
    struct block_list *bl = NULL;
    int rid = script_getnum(st, 2);
    if ((bl = map_id2bl(rid)))
    {
        switch(bl->type)
        {
            case BL_MOB: script_pushstrcopy(st, ((TBL_MOB*)bl)->name); break;
            case BL_PC:  script_pushstrcopy(st, ((TBL_PC*)bl)->status.name); break;
            case BL_NPC: script_pushstrcopy(st, ((TBL_NPC*)bl)->exname); break;
            default:
                ShowError("buildin_rid2name: BL type unknown.\n");
                script_pushconststr(st, "");
                break;
        }
    }
    else
    {
        ShowError("buildin_rid2name: invalid RID\n");
        script_pushconststr(st, "(null)");
    }
    return 0;
}

BUILDIN_FUNC(setnpcclass)
{
    int  newsprite;
    struct npc_data *nd = 0;

    if (script_hasdata(st, 3))
    {
        nd = npc_name2id (script_getstr(st, 2));
        newsprite = script_getnum(st, 3);
    }
    else if (script_hasdata(st, 2))
    {
        if (!st->oid)
            return 1;

        nd = (struct npc_data *) map_id2bl (st->oid);
        newsprite = script_getnum(st, 2);
    }
    if (!nd)
        return 1;

    nd->class = newsprite;
    npc_enable (nd->name, 0);
    npc_enable (nd->name, 1);

    return 0;
}

BUILDIN_FUNC(settempnpcclass)
{
    int  newsprite;
    int  oldsprite;
    struct npc_data *nd = 0;

    if (script_hasdata(st, 3))
    {
        nd = npc_name2id (script_getstr(st, 2));
        newsprite = script_getnum(st, 3);
    }
    else if (script_hasdata(st, 2))
    {
        if (!st->oid)
            return 1;

        nd = (struct npc_data *) map_id2bl (st->oid);
        newsprite = script_getnum(st, 2);
    }
    if (!nd)
        return 1;

    oldsprite = nd->class;
    nd->class = newsprite;
    npc_enable (nd->name, 0);
    npc_enable (nd->name, 1);
    nd->class = oldsprite;
    return 0;
}

BUILDIN_FUNC(getnpcclass)
{
    struct npc_data *nd = 0;

    if (script_hasdata(st, 2))
    {
        nd = npc_name2id (script_getstr(st, 2));
    }
    if (!nd && !st->oid)
    {
        script_pushint(st, -1);
        return 0;
    }

    if (!nd)
        nd = (struct npc_data *) map_id2bl (st->oid);

    if (!nd)
    {
        script_pushint(st, -1);
        return 0;
    }

    script_pushint(st, (int)nd->class);

    return 0;
}

BUILDIN_FUNC(l)
{
    char *str = script_getstr(st, 2);
    TBL_PC *sd = script_rid2sd(st);

    script_pushstr(st, (unsigned char *)strdup(lang_pctrans (str, sd)));
    return 0;
}

BUILDIN_FUNC(getlang)
{
    TBL_PC *sd;

    if (script_hasdata(st, 2))
    {
        sd = map_nick2sd(script_getstr(st, 2));
    }
    else
    {
        sd = script_rid2sd(st);
    }
    if (!sd)
    {
        script_pushint(st, -1);
        return 0;
    }

    script_pushint(st, sd->status.language);
    return 0;
}

BUILDIN_FUNC(setlang)
{
    TBL_PC *sd = 0;

    if (script_hasdata(st, 3))
    {
        sd = map_nick2sd(script_getstr(st, 3));
    }
    else
    {
        sd = script_rid2sd(st);
    }

    if (!sd)
        return 0;

    pc_set_lang (sd, script_getnum(st, 2));
    return 0;
}

BUILDIN_FUNC(seta)
{
    unsigned var;
    char *varname;
    int idx;
    int val;
    TBL_PC *sd = NULL;

    int  num = st->stack->stack_data[st->start + 2].u.num;
    varname = str_buf + str_data[num & 0x00ffffff].str;

    if (!varname || strlen(varname) < 1 || varname[strlen(varname) - 1] == '$')
        return 0;

    if (not_server_variable(*varname))
    {
        sd = script_rid2sd(st);
        if (sd == NULL)
        {
            ShowError("script:seta: no player attached for player variable '%s'\n", varname);
            return 0;
        }
    }

    var = (unsigned)get_val2(st, add_str(varname));

    idx = script_getnum(st, 3);
    if (idx < 0 || idx > 31)
        return 0;

    val = script_getnum(st, 4);
    if (val < 0)
        val = 0;
    else if (val > 1)
        val = 1;

    if (val)
    {
        var |= 1 << idx;
    }
    else
    {
        var |= 1 << idx;
        var -= 1 << idx;
    }

    setd_sub(sd, varname, 0, (void *)var);

    return 0;
}

BUILDIN_FUNC(geta)
{
    unsigned var;
    char *varname;
    int idx;

    int  num = st->stack->stack_data[st->start + 2].u.num;
    varname = str_buf + str_data[num & 0x00ffffff].str;

    if (!varname || strlen(varname) < 1 || varname[strlen(varname) - 1] == '$')
    {
        script_pushint(st, 0);
        return 0;
    }

    var = (unsigned)get_val2(st, add_str(varname));

    idx = script_getnum(st, 3);
    if (idx < 0 || idx > 31)
    {
        script_pushint(st, 0);
        return 0;
    }

    if (var & (1 << idx))
        script_pushint(st, 1);
    else
        script_pushint(st, 0);

    return 0;
}


BUILDIN_FUNC(seta2)
{
    unsigned var;
    char *varname;
    int idx;
    int val;
    int tmp;
    TBL_PC *sd = NULL;

    int  num = st->stack->stack_data[st->start + 2].u.num;
    varname = str_buf + str_data[num & 0x00ffffff].str;

    if (!varname || strlen(varname) < 1 || varname[strlen(varname) - 1] == '$')
        return 0;

    if (not_server_variable(*varname))
    {
        sd = script_rid2sd(st);
        if (sd == NULL)
        {
            ShowError("script:seta: no player attached for player variable '%s'\n", varname);
            return 0;
        }
    }

    var = (unsigned)get_val2(st, add_str(varname));

    idx = script_getnum(st, 3);
    if (idx < 0 || idx > 15)
        return 0;

    val = script_getnum(st, 4);
    if (val < 0)
        val = 0;
    else if (val > 3)
        val = 3;

    tmp = var & (3 << (idx * 2));
    var ^= tmp;

    var = (var | (val << (idx * 2)));

    setd_sub(sd, varname, 0, (void *)var);

    return 0;
}

BUILDIN_FUNC(geta2)
{
    unsigned var;
    char *varname;
    int idx;
    int val;

    int  num = st->stack->stack_data[st->start + 2].u.num;
    varname = str_buf + str_data[num & 0x00ffffff].str;

    if (!varname || strlen(varname) < 1 || varname[strlen(varname) - 1] == '$')
    {
        script_pushint(st, 0);
        return 0;
    }

    var = (unsigned)get_val2(st, add_str(varname));

    idx = script_getnum(st, 3);
    if (idx < 0 || idx > 15)
    {
        script_pushint(st, 0);
        return 0;
    }

    val = (var & (3 << (idx * 2))) >> (idx * 2);
    script_pushint(st, val);

    return 0;
}

BUILDIN_FUNC(seta4)
{
    unsigned var;
    char *varname;
    int idx;
    int val;
    int tmp;
    TBL_PC *sd = NULL;

    int  num = st->stack->stack_data[st->start + 2].u.num;
    varname = str_buf + str_data[num & 0x00ffffff].str;

    if (!varname || strlen(varname) < 1 || varname[strlen(varname) - 1] == '$')
        return 0;

    if (not_server_variable(*varname))
    {
        sd = script_rid2sd(st);
        if (sd == NULL)
        {
            ShowError("script:seta: no player attached for player variable '%s'\n", varname);
            return 0;
        }
    }

    var = (unsigned)get_val2(st, add_str(varname));

    idx = script_getnum(st, 3);
    if (idx < 0 || idx > 7)
        return 0;

    val = script_getnum(st, 4);
    if (val < 0)
        val = 0;
    else if (val > 15)
        val = 15;

    tmp = var & (15 << (idx * 4));
    var ^= tmp;

    var = (var | (val << (idx * 4)));

    setd_sub(sd, varname, 0, (void *)var);

    return 0;
}

BUILDIN_FUNC(geta4)
{
    unsigned var;
    char *varname;
    int idx;
    int val;

    int  num = st->stack->stack_data[st->start + 2].u.num;
    varname = str_buf + str_data[num & 0x00ffffff].str;

    if (!varname || strlen(varname) < 1 || varname[strlen(varname) - 1] == '$')
    {
        script_pushint(st, 0);
        return 0;
    }

    var = (unsigned)get_val2(st, add_str(varname));

    idx = script_getnum(st, 3);
    if (idx < 0 || idx > 7)
    {
        script_pushint(st, 0);
        return 0;
    }

    val = (var & (15 << (idx * 4))) >> (idx * 4);
    script_pushint(st, val);

    return 0;
}

BUILDIN_FUNC(seta8)
{
    unsigned var;
    char *varname;
    int idx;
    int val;
    int tmp;
    TBL_PC *sd = NULL;

    int  num = st->stack->stack_data[st->start + 2].u.num;
    varname = str_buf + str_data[num & 0x00ffffff].str;

    if (!varname || strlen(varname) < 1 || varname[strlen(varname) - 1] == '$')
        return 0;

    if (not_server_variable(*varname))
    {
        sd = script_rid2sd(st);
        if (sd == NULL)
        {
            ShowError("script:seta: no player attached for player variable '%s'\n", varname);
            return 0;
        }
    }

    var = (unsigned)get_val2(st, add_str(varname));

    idx = script_getnum(st, 3);
    if (idx < 0 || idx > 3)
        return 0;

    val = script_getnum(st, 4);
    if (val < 0)
        val = 0;
    else if (val > 255)
        val = 255;

    tmp = var & (255 << (idx * 8));
    var ^= tmp;

    var = (var | (val << (idx * 8)));

    setd_sub(sd, varname, 0, (void *)var);

    return 0;
}

BUILDIN_FUNC(geta8)
{
    unsigned var;
    char *varname;
    int idx;
    int val;

    int  num = st->stack->stack_data[st->start + 2].u.num;
    varname = str_buf + str_data[num & 0x00ffffff].str;

    if (!varname || strlen(varname) < 1 || varname[strlen(varname) - 1] == '$')
    {
        script_pushint(st, 0);
        return 0;
    }

    var = (unsigned)get_val2(st, add_str(varname));

    idx = script_getnum(st, 3);
    if (idx < 0 || idx > 3)
    {
        script_pushint(st, 0);
        return 0;
    }

    val = (var & (255 << (idx * 8))) >> (idx * 8);
    script_pushint(st, val);

    return 0;
}

BUILDIN_FUNC(seta16)
{
    unsigned var;
    char *varname;
    int idx;
    int val;
    int tmp;
    TBL_PC *sd = NULL;

    int  num = st->stack->stack_data[st->start + 2].u.num;
    varname = str_buf + str_data[num & 0x00ffffff].str;

    if (!varname || strlen(varname) < 1 || varname[strlen(varname) - 1] == '$')
        return 0;

    if (not_server_variable(*varname))
    {
        sd = script_rid2sd(st);
        if (sd == NULL)
        {
            ShowError("script:seta: no player attached for player variable '%s'\n", varname);
            return 0;
        }
    }

    var = (unsigned)get_val2(st, add_str(varname));

    idx = script_getnum(st, 3);
    if (idx < 0 || idx > 1)
        return 0;

    val = script_getnum(st, 4);
    if (val < 0)
        val = 0;
    else if (val > 65535)
        val = 65535;

    tmp = var & (65535 << (idx * 16));
    var ^= tmp;

    var = (var | (val << (idx * 16)));

    setd_sub(sd, varname, 0, (void *)var);

    return 0;
}

BUILDIN_FUNC(geta16)
{
    unsigned var;
    char *varname;
    int idx;
    int val;

    int  num = st->stack->stack_data[st->start + 2].u.num;
    varname = str_buf + str_data[num & 0x00ffffff].str;

    if (!varname || strlen(varname) < 1 || varname[strlen(varname) - 1] == '$')
    {
        script_pushint(st, 0);
        return 0;
    }

    var = (unsigned)get_val2(st, add_str(varname));

    idx = script_getnum(st, 3);
    if (idx < 0 || idx > 1)
    {
        script_pushint(st, 0);
        return 0;
    }

    val = (var & (65535 << (idx * 16))) >> (idx * 16);
    script_pushint(st, val);

    return 0;
}

BUILDIN_FUNC(getclientversion)
{
    TBL_PC *sd = NULL;

    sd = script_rid2sd(st);
    if (sd == NULL)
    {
        script_pushint(st, 0);
        return 0;
    }

    script_pushint(st, sd->tmw_version);
    return 0;
}


BUILDIN_FUNC(g)
{
    TBL_PC *sd = NULL;

    sd = script_rid2sd(st);
    if (sd == NULL || !sd->sex)
        script_pushstr(st, (unsigned char *)strdup(script_getstr(st, 2)));
    else
        script_pushstr(st, (unsigned char *)strdup(script_getstr(st, 3)));

    return 0;
}


//
// 実行部main 
//
/*==========================================
 * コマンドの読み取り 
 *------------------------------------------
 */
static int unget_com_data = -1;
int get_com (unsigned char *script, int *pos)
{
    int  i, j;
    if (unget_com_data >= 0)
    {
        i = unget_com_data;
        unget_com_data = -1;
        return i;
    }
    if (script[*pos] >= 0x80)
    {
        return C_INT;
    }
    i = 0;
    j = 0;
    while (script[*pos] >= 0x40)
    {
        i = script[(*pos)++] << j;
        j += 6;
    }
    return i + (script[(*pos)++] << j);
}

/*==========================================
 * コマンドのプッシュバック 
 *------------------------------------------
 */
void unget_com (int c)
{
    if (unget_com_data != -1)
    {
        if (battle_config.error_log)
            printf ("unget_com can back only 1 data\n");
    }
    unget_com_data = c;
}

/*==========================================
 * 数値の所得 
 *------------------------------------------
 */
int get_num (unsigned char *script, int *pos)
{
    int  i, j;
    i = 0;
    j = 0;
    while (script[*pos] >= 0xc0)
    {
        i += (script[(*pos)++] & 0x7f) << j;
        j += 6;
    }
    return i + ((script[(*pos)++] & 0x7f) << j);
}

/*==========================================
 * スタックから値を取り出す 
 *------------------------------------------
 */
int pop_val (struct script_state *st)
{
    if (st->stack->sp <= 0)
        return 0;
    st->stack->sp--;
    get_val (st, &(st->stack->stack_data[st->stack->sp]));
    if (st->stack->stack_data[st->stack->sp].type == C_INT)
        return st->stack->stack_data[st->stack->sp].u.num;
    return 0;
}

#define isstr(c) ((c).type==C_STR || (c).type==C_CONSTSTR)

/*==========================================
 * 加算演算子 
 *------------------------------------------
 */
void op_add (struct script_state *st)
{
    st->stack->sp--;
    get_val (st, &(st->stack->stack_data[st->stack->sp]));
    get_val (st, &(st->stack->stack_data[st->stack->sp - 1]));

    if (isstr (st->stack->stack_data[st->stack->sp])
        || isstr (st->stack->stack_data[st->stack->sp - 1]))
    {
        conv_str (st, &(st->stack->stack_data[st->stack->sp]));
        conv_str (st, &(st->stack->stack_data[st->stack->sp - 1]));
    }
    if (st->stack->stack_data[st->stack->sp].type == C_INT)
    {                           // ii
        st->stack->stack_data[st->stack->sp - 1].u.num +=
            st->stack->stack_data[st->stack->sp].u.num;
    }
    else
    {                           // ssの予定 
        char *buf;
        buf =
            (char *)
            aCalloc (strlen (st->stack->stack_data[st->stack->sp - 1].u.str) +
                     strlen (st->stack->stack_data[st->stack->sp].u.str) + 1,
                     sizeof (char));
        strcpy (buf, st->stack->stack_data[st->stack->sp - 1].u.str);
        strcat (buf, st->stack->stack_data[st->stack->sp].u.str);
        if (st->stack->stack_data[st->stack->sp - 1].type == C_STR)
        {
            free (st->stack->stack_data[st->stack->sp - 1].u.str);
            st->stack->stack_data[st->stack->sp - 1].u.str = 0;
        }
        if (st->stack->stack_data[st->stack->sp].type == C_STR)
        {
            free (st->stack->stack_data[st->stack->sp].u.str);
            st->stack->stack_data[st->stack->sp].u.str = 0;
        }
        st->stack->stack_data[st->stack->sp - 1].type = C_STR;
        st->stack->stack_data[st->stack->sp - 1].u.str = buf;
    }
}

/*==========================================
 * 二項演算子(文字列) 
 *------------------------------------------
 */
void op_2str (struct script_state *st, int op, int sp1, int sp2)
{
    char *s1 = st->stack->stack_data[sp1].u.str,
        *s2 = st->stack->stack_data[sp2].u.str;
    int  a = 0;

    switch (op)
    {
        case C_EQ:
            a = (strcmp (s1, s2) == 0);
            break;
        case C_NE:
            a = (strcmp (s1, s2) != 0);
            break;
        case C_GT:
            a = (strcmp (s1, s2) > 0);
            break;
        case C_GE:
            a = (strcmp (s1, s2) >= 0);
            break;
        case C_LT:
            a = (strcmp (s1, s2) < 0);
            break;
        case C_LE:
            a = (strcmp (s1, s2) <= 0);
            break;
        default:
            printf ("illegal string operater\n");
            break;
    }

    push_val (st->stack, C_INT, a);

    if (st->stack->stack_data[sp1].type == C_STR)
        free (s1);
    if (st->stack->stack_data[sp2].type == C_STR)
        free (s2);
}

/*==========================================
 * 二項演算子(数値) 
 *------------------------------------------
 */
void op_2num (struct script_state *st, int op, int i1, int i2)
{
    switch (op)
    {
        case C_SUB:
            i1 -= i2;
            break;
        case C_MUL:
            i1 *= i2;
            break;
        case C_DIV:
            if (i2)
                i1 /= i2;
            else
                i1 = 0;
            break;
        case C_MOD:
            if (i2)
                i1 %= i2;
            else
                i1 = 0;
            break;
        case C_AND:
            i1 &= i2;
            break;
        case C_OR:
            i1 |= i2;
            break;
        case C_XOR:
            i1 ^= i2;
            break;
        case C_LAND:
            i1 = i1 && i2;
            break;
        case C_LOR:
            i1 = i1 || i2;
            break;
        case C_EQ:
            i1 = i1 == i2;
            break;
        case C_NE:
            i1 = i1 != i2;
            break;
        case C_GT:
            i1 = i1 > i2;
            break;
        case C_GE:
            i1 = i1 >= i2;
            break;
        case C_LT:
            i1 = i1 < i2;
            break;
        case C_LE:
            i1 = i1 <= i2;
            break;
        case C_R_SHIFT:
            i1 = i1 >> i2;
            break;
        case C_L_SHIFT:
            i1 = i1 << i2;
            break;
        default:
            break;
    }
    push_val (st->stack, C_INT, i1);
}

/*==========================================
 * 二項演算子 
 *------------------------------------------
 */
void op_2 (struct script_state *st, int op)
{
    int  i1, i2;
    char *s1 = NULL, *s2 = NULL;

    i2 = pop_val (st);
    if (isstr (st->stack->stack_data[st->stack->sp]))
        s2 = st->stack->stack_data[st->stack->sp].u.str;

    i1 = pop_val (st);
    if (isstr (st->stack->stack_data[st->stack->sp]))
        s1 = st->stack->stack_data[st->stack->sp].u.str;

    if (s1 != NULL && s2 != NULL)
    {
        // ss => op_2str
        op_2str (st, op, st->stack->sp, st->stack->sp + 1);
    }
    else if (s1 == NULL && s2 == NULL)
    {
        // ii => op_2num
        op_2num (st, op, i1, i2);
    }
    else
    {
        // si,is => error
        printf ("script: op_2: int&str, str&int not allow.");
        push_val (st->stack, C_INT, 0);
    }
}

/*==========================================
 * 単項演算子 
 *------------------------------------------
 */
void op_1num (struct script_state *st, int op)
{
    int  i1;
    i1 = pop_val (st);
    switch (op)
    {
        case C_NEG:
            i1 = -i1;
            break;
        case C_NOT:
            i1 = ~i1;
            break;
        case C_LNOT:
            i1 = !i1;
            break;
        default:
            break;
    }
    push_val (st->stack, C_INT, i1);
}

/*==========================================
 * 関数の実行 
 *------------------------------------------
 */
int run_func (struct script_state *st)
{
    int  i, start_sp, end_sp, func;

    end_sp = st->stack->sp;
    for (i = end_sp - 1; i >= 0 && st->stack->stack_data[i].type != C_ARG;
         i--);
    if (i == 0)
    {
        if (battle_config.error_log)
            printf ("function not found\n");
//      st->stack->sp=0;
        st->state = END;
        return 0;
    }
    start_sp = i - 1;
    st->start = i - 1;
    st->end = end_sp;

    func = st->stack->stack_data[st->start].u.num;
    if (st->stack->stack_data[st->start].type != C_NAME
        || str_data[func].type != C_FUNC)
    {
        printf ("run_func: not function and command! \n");
//      st->stack->sp=0;
        st->state = END;
        return 0;
    }
#ifdef DEBUG_RUN
    if (battle_config.etc_log)
    {
        printf ("run_func : %s? (%d(%d))\n", str_buf + str_data[func].str,
                func, str_data[func].type);
        printf ("stack dump :");
        for (i = 0; i < end_sp; i++)
        {
            switch (st->stack->stack_data[i].type)
            {
                case C_INT:
                    printf (" int(%d)", st->stack->stack_data[i].u.num);
                    break;
                case C_NAME:
                    printf (" name(%s)",
                            str_buf +
                            str_data[st->stack->stack_data[i].u.num].str);
                    break;
                case C_ARG:
                    printf (" arg");
                    break;
                case C_POS:
                    printf (" pos(%d)", st->stack->stack_data[i].u.num);
                    break;
                default:
                    printf (" %d,%d", st->stack->stack_data[i].type,
                            st->stack->stack_data[i].u.num);
            }
        }
        printf ("\n");
    }
#endif
    if (str_data[func].func)
    {
        str_data[func].func (st);
    }
    else
    {
        if (battle_config.error_log)
            printf ("run_func : %s? (%d(%d))\n", str_buf + str_data[func].str,
                    func, str_data[func].type);
        push_val (st->stack, C_INT, 0);
    }

    pop_stack (st->stack, start_sp, end_sp);

    if (st->state == RETFUNC)
    {
          // ユーザー定義関数からの復帰 
        int  olddefsp = st->defsp;
        int  i;

        pop_stack (st->stack, st->defsp, start_sp); // 復帰に邪魔なスタック削除 
        if (st->defsp < 4
            || st->stack->stack_data[st->defsp - 1].type != C_RETINFO)
        {
            printf
                ("script:run_func(return) return without callfunc or callsub!\n");
            st->state = END;
            return 0;
        }
        i = conv_num (st, &(st->stack->stack_data[st->defsp - 4]));                     // 引数の数所得 
        st->pos = conv_num (st, &(st->stack->stack_data[st->defsp - 1]));               // スクリプト位置の復元 
        st->script = (char *) conv_num (st, &(st->stack->stack_data[st->defsp - 2]));   // スクリプトを復元 
        st->defsp = conv_num (st, &(st->stack->stack_data[st->defsp - 3]));             // 基準スタックポインタを復元 

        pop_stack (st->stack, olddefsp - 4 - i, olddefsp);                              // 要らなくなったスタック(引数と復帰用データ)削除 

        st->state = GOTO;
    }

    return 0;
}

/*==========================================
 * スクリプトの実行メイン部分 
 *------------------------------------------
 */
int run_script_main (unsigned char *script, int pos,
                     int rid __attribute__ ((unused)),
                     int oid __attribute__ ((unused)),
                     struct script_state *st, unsigned char *rootscript)
{
    int  c, rerun_pos;
    int  cmdcount = script_config.check_cmdcount;
    int  gotocount = script_config.check_gotocount;
    struct script_stack *stack = st->stack;

    st->defsp = stack->sp;
    st->script = script;

    rerun_pos = st->pos;
    for (st->state = 0; st->state == 0;)
    {
        switch (c = get_com (script, &st->pos))
        {
            case C_EOL:
                if (stack->sp != st->defsp)
                {
                    if (battle_config.error_log)
                        printf ("stack.sp(%d) != default(%d)\n", stack->sp,
                                st->defsp);
                    stack->sp = st->defsp;
                }
                rerun_pos = st->pos;
                break;
            case C_INT:
                push_val (stack, C_INT, get_num (script, &st->pos));
                break;
            case C_POS:
            case C_NAME:
                push_val (stack, c, (*(int *) (script + st->pos)) & 0xffffff);
                st->pos += 3;
                break;
            case C_ARG:
                push_val (stack, c, 0);
                break;
            case C_STR:
                push_str (stack, C_CONSTSTR, script + st->pos);
                while (script[st->pos++]);
                break;
            case C_FUNC:
                run_func (st);
                if (st->state == GOTO)
                {
                    rerun_pos = st->pos;
                    script = st->script;
                    st->state = 0;
                    if (gotocount > 0 && (--gotocount) <= 0)
                    {
                        printf ("run_script: infinity loop !\n");
                        st->state = END;
                    }
                }
                break;

            case C_ADD:
                op_add (st);
                break;

            case C_SUB:
            case C_MUL:
            case C_DIV:
            case C_MOD:
            case C_EQ:
            case C_NE:
            case C_GT:
            case C_GE:
            case C_LT:
            case C_LE:
            case C_AND:
            case C_OR:
            case C_XOR:
            case C_LAND:
            case C_LOR:
            case C_R_SHIFT:
            case C_L_SHIFT:
                op_2 (st, c);
                break;

            case C_NEG:
            case C_NOT:
            case C_LNOT:
                op_1num (st, c);
                break;

            case C_NOP:
                st->state = END;
                break;

            default:
                if (battle_config.error_log)
                    printf ("unknown command : %d @ %d\n", c, pos);
                st->state = END;
                break;
        }
        if (cmdcount > 0 && (--cmdcount) <= 0)
        {
            printf ("run_script: infinity loop !\n");
            st->state = END;
        }
    }
    switch (st->state)
    {
        case STOP:
            break;
        case END:
        {
            struct map_session_data *sd = map_id2sd (st->rid);
            st->pos = -1;
            if (sd && sd->npc_id == st->oid)
                npc_event_dequeue (sd);
        }
            break;
        case RERUNLINE:
        {
            st->pos = rerun_pos;
        }
            break;
    }

    if (st->state != END)
    {
        // 再開するためにスタック情報を保存 
        struct map_session_data *sd = map_id2sd (st->rid);
        if (sd /* && sd->npc_stackbuf==NULL */ )
        {
            free (sd->npc_stackbuf);
            sd->npc_stackbuf =
                (char *) aCalloc (sizeof (stack->stack_data[0]) *
                                  stack->sp_max, sizeof (char));
            if (!sd->npc_stackbuf)
            {
                printf("out of memory run_script_main");
                exit(1);
            }
            memcpy (sd->npc_stackbuf, stack->stack_data,
                    sizeof (stack->stack_data[0]) * stack->sp_max);
            sd->npc_stack = stack->sp;
            sd->npc_stackmax = stack->sp_max;
            sd->npc_script = script;
            sd->npc_scriptroot = rootscript;
        }
    }

    return 0;
}

/*==========================================
 * スクリプトの実行 
 *------------------------------------------
 */
int run_script (unsigned char *script, int pos, int rid, int oid)
{
    return run_script_l (script, pos, rid, oid, 0, NULL);
}

int run_script_l (unsigned char *script, int pos, int rid, int oid,
                  int args_nr, argrec_t * args)
{
    struct script_stack stack;
    struct script_state st;
    struct map_session_data *sd = map_id2sd (rid);
    unsigned char *rootscript = script;
    int  i;
    if (script == NULL || pos < 0)
        return -1;

    if (sd && sd->npc_stackbuf && sd->npc_scriptroot == (char *) rootscript)
    {
        // 前回のスタックを復帰 
        script = sd->npc_script;
        stack.sp = sd->npc_stack;
        stack.sp_max = sd->npc_stackmax;
        stack.stack_data =
            (struct script_data *) aCalloc (stack.sp_max,
                                            sizeof (stack.stack_data[0]));
        memcpy (stack.stack_data, sd->npc_stackbuf,
                sizeof (stack.stack_data[0]) * stack.sp_max);
        free (sd->npc_stackbuf);
        sd->npc_stackbuf = NULL;
    }
    else
    {
           // スタック初期化 
        stack.sp = 0;
        stack.sp_max = 64;
        stack.stack_data =
            (struct script_data *) aCalloc (stack.sp_max,
                                            sizeof (stack.stack_data[0]));
    }
    st.stack = &stack;
    st.pos = pos;
    st.rid = rid;
    st.oid = oid;
    for (i = 0; i < args_nr; i++)
    {
        if (args[i].name[strlen (args[i].name) - 1] == '$')
            pc_setregstr (sd, add_str (args[i].name), args[i].v.s);
        else
            pc_setreg (sd, add_str (args[i].name), args[i].v.i);
    }
    run_script_main (script, pos, rid, oid, &st, rootscript);

    free (stack.stack_data);
    stack.stack_data = NULL;
    return st.pos;
}

/*==========================================
 * マップ変数の変更 
 *------------------------------------------
 */
int mapreg_setreg (int num, int val)
{
    if (val != 0)
        numdb_insert (mapreg_db, num, val);
    else
        numdb_erase (mapreg_db, num);

    mapreg_dirty = 1;
    return 0;
}

/*==========================================
 * 文字列型マップ変数の変更 
 *------------------------------------------
 */
int mapreg_setregstr (int num, const char *str)
{
    char *p;

    p = numdb_search (mapregstr_db, num);
    free (p);

    if (str == NULL || *str == 0)
    {
        numdb_erase (mapregstr_db, num);
        mapreg_dirty = 1;
        return 0;
    }
    p = (char *) aCalloc (strlen (str) + 1, sizeof (char));
    strcpy (p, str);
    numdb_insert (mapregstr_db, num, p);
    mapreg_dirty = 1;
    return 0;
}

/*==========================================
 * 永続的マップ変数の読み込み 
 *------------------------------------------
 */
static int script_load_mapreg ()
{
    FILE *fp;
    char line[1024];

    if ((fp = fopen_ (mapreg_txt, "rt")) == NULL)
        return -1;

    while (fgets (line, sizeof (line), fp))
    {
        char buf1[256], buf2[1024], *p;
        int  n, v, s, i;
        if (sscanf (line, "%255[^,],%d\t%n", buf1, &i, &n) != 2 &&
            (i = 0, sscanf (line, "%[^\t]\t%n", buf1, &n) != 1))
            continue;
        if (buf1[strlen (buf1) - 1] == '$')
        {
            if (sscanf (line + n, "%255[^\n\r]", buf2) != 1)
            {
                printf ("%s: %s broken data !\n", mapreg_txt, buf1);
                continue;
            }
            p = (char *) aCalloc (strlen (buf2) + 1, sizeof (char));
            strcpy (p, buf2);
            s = add_str (buf1);
            numdb_insert (mapregstr_db, (i << 24) | s, p);
        }
        else
        {
            if (sscanf (line + n, "%d", &v) != 1)
            {
                printf ("%s: %s broken data !\n", mapreg_txt, buf1);
                continue;
            }
            s = add_str (buf1);
            numdb_insert (mapreg_db, (i << 24) | s, v);
        }
    }
    fclose_ (fp);
    mapreg_dirty = 0;
    return 0;
}

/*==========================================
 * 永続的マップ変数の書き込み 
 *------------------------------------------
 */
static int script_save_mapreg_intsub (void *key, void *data, va_list ap)
{
    FILE *fp = va_arg (ap, FILE *);
    int  num = ((int) key) & 0x00ffffff, i = ((int) key) >> 24;
    char *name = str_buf + str_data[num].str;
    if (name[1] != '@')
    {
        if (i == 0)
            fprintf (fp, "%s\t%d\n", name, (int) data);
        else
            fprintf (fp, "%s,%d\t%d\n", name, i, (int) data);
    }
    return 0;
}

static int script_save_mapreg_strsub (void *key, void *data, va_list ap)
{
    FILE *fp = va_arg (ap, FILE *);
    int  num = ((int) key) & 0x00ffffff, i = ((int) key) >> 24;
    char *name = str_buf + str_data[num].str;
    if (name[1] != '@')
    {
        if (i == 0)
            fprintf (fp, "%s\t%s\n", name, (char *) data);
        else
            fprintf (fp, "%s,%d\t%s\n", name, i, (char *) data);
    }
    return 0;
}

static int script_save_mapreg ()
{
    FILE *fp;
    int  lock;

    if ((fp = lock_fopen (mapreg_txt, &lock, &index_db)) == NULL)
        return -1;
    numdb_foreach (mapreg_db, script_save_mapreg_intsub, fp);
    numdb_foreach (mapregstr_db, script_save_mapreg_strsub, fp);
    lock_fclose (fp, mapreg_txt, &lock, &index_db);
    mapreg_dirty = 0;

    index_db ++;
    if (index_db > script_config.db_count)
        index_db = 0;
    return 0;
}

static int script_autosave_mapreg (int tid __attribute__ ((unused)),
                                   unsigned int tick __attribute__ ((unused)),
                                   int id __attribute__ ((unused)),
                                   int data __attribute__ ((unused)))
{
    if (mapreg_dirty)
        script_save_mapreg ();
    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
static int set_posword (char *p)
{
    char *np, *str[15];
    int  i = 0;
    for (i = 0; i < 11; i++)
    {
        if ((np = strchr (p, ',')) != NULL)
        {
            str[i] = p;
            *np = 0;
            p = np + 1;
        }
        else
        {
            str[i] = p;
            p += strlen (p);
        }
        if (str[i])
            strcpy (pos[i], str[i]);
    }
    return 0;
}

int script_config_read (char *cfgName)
{
    int  i;
    char line[1024], w1[1024], w2[1024];
    FILE *fp;

    script_config.warn_func_no_comma = 1;
    script_config.warn_cmd_no_comma = 1;
    script_config.warn_func_mismatch_paramnum = 1;
    script_config.warn_cmd_mismatch_paramnum = 1;
    script_config.check_cmdcount = 8192;
    script_config.check_gotocount = 512;
    script_config.db_count = 5*2;

    fp = fopen_ (cfgName, "r");
    if (fp == NULL)
    {
        printf ("file not found: %s\n", cfgName);
        return 1;
    }
    while (fgets (line, 1020, fp))
    {
        if (line[0] == '/' && line[1] == '/')
            continue;
        i = sscanf (line, "%1000[^:]: %1000[^\r\n]", w1, w2);
        if (i != 2)
            continue;
        if (strcmpi (w1, "refine_posword") == 0)
        {
            set_posword (w2);
        }
        else if (strcmpi (w1, "db_count") == 0)
        {
            script_config.db_count = atoi(w2) * 2;
        }
        else if (strcmpi (w1, "import") == 0)
        {
            script_config_read (w2);
        }
    }
    fclose_ (fp);

    return 0;
}

/*==========================================
 * 終了 
 *------------------------------------------
 */
static int mapreg_db_final (void *key __attribute__ ((unused)),
                            void *data __attribute__ ((unused)),
                            va_list ap __attribute__ ((unused)))
{
    return 0;
}

static int mapregstr_db_final (void *key __attribute__ ((unused)),
                               void *data, va_list ap __attribute__ ((unused)))
{
    free (data);
    return 0;
}

static int scriptlabel_db_final (void *key __attribute__ ((unused)),
                                 void *data __attribute__ ((unused)),
                                 va_list ap __attribute__ ((unused)))
{
    return 0;
}

static int userfunc_db_final (void *key, void *data,
                              va_list ap __attribute__ ((unused)))
{
    free (key);
    free (data);
    return 0;
}

int do_final_script ()
{
    if (mapreg_dirty >= 0)
        script_save_mapreg ();
    free (script_buf);
    script_buf = 0;

    if (mapreg_db)
        numdb_final (mapreg_db, mapreg_db_final);
    if (mapregstr_db)
        strdb_final (mapregstr_db, mapregstr_db_final);
    if (scriptlabel_db)
        strdb_final (scriptlabel_db, scriptlabel_db_final);
    if (userfunc_db)
        strdb_final (userfunc_db, userfunc_db_final);

    free (str_data);
    str_data = 0;
    free (str_buf);
    str_buf = 0;

    return 0;
}

/*==========================================
 * 初期化 
 *------------------------------------------
 */
int do_init_script ()
{
    mapreg_db = numdb_init ();
    mapregstr_db = numdb_init ();
    script_load_mapreg ();

    add_timer_func_list (script_autosave_mapreg, "script_autosave_mapreg");
    add_timer_interval (gettick () + MAPREG_AUTOSAVE_INTERVAL,
                        script_autosave_mapreg, 0, 0,
                        MAPREG_AUTOSAVE_INTERVAL);

    scriptlabel_db = strdb_init (50);
    return 0;
}
