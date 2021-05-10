/*************************************************************************
 *
 *  Copyright (c) 2021 Rajit Manohar
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA  02110-1301, USA.
 *
 **************************************************************************
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <common/config.h>
#include <common/misc.h>
#include <common/atrace.h>
#include "liberty.h"

static int is_xyce (void)
{
  if (strstr (config_get_string ("xcell.spice_binary"), "Xyce")) {
    return 1;
  }
  return 0;
}

static int is_hspice (void)
{
  return !is_xyce();
}

static void unlink_files (const char *s, const char *ext[])
{
  char buf[1024];
  int i = 0;

  while (ext[i]) {
    snprintf (buf, 1024, "%s.%s", s, ext[i]);
    unlink (buf);
    i++;
  }
}

static void unlink_hspice (const char *s)
{
  const char *ext[] = { "mt0", "st0", "tr0", "pa0", "ic0", NULL };
  unlink_files (s, ext);
}

static void unlink_xyce (const char *s)
{
  const char *ext[] = { "spi.mt0", "spi.raw", NULL };
  unlink_files (s, ext);
}

static void unlink_generic (const char *s)
{
  const char *ext[] = { "spi", "log", NULL };
  unlink_files (s, ext);

  if (is_xyce ()) {
    unlink_xyce (s);
  }
  else if (is_hspice()) {
    unlink_hspice (s);
  }
}

static void unlink_generic_trace (const char *s)
{
  const char *ext[] = { "trace", "names", NULL };

  unlink_generic (s);
  unlink_files (s, ext);
}

static struct Hashtable *parse_measurements (const char *s, const char *param = NULL, int skip = 0)
{
  FILE *fp;
  char buf[1024];
  struct Hashtable *H;
  hash_bucket_t *b;
  int is_skip = 0;

  fp = fopen (s, "r");
  if (!fp) {
    return NULL;
  }

  H = hash_new (8);

  if (skip > 0 && param) {
    is_skip = 1;
  }
  
  while (fgets (buf, 1024, fp)) {
    double v;
    char *s;

    if (buf[0] == '.' || buf[0] == '$' || buf[0] == '#') {
      continue;
    }
    s = strtok (buf, " \t");

    if (!s) {
      continue;
    }

    if (is_skip && (strcmp (s, param) == 0)) {
      if (skip > 0) {
	skip--;
	continue;
      }
      is_skip = 0;
    }
    else if (is_skip) {
      continue;
    }
    else if (param && (strcmp (s, param) == 0)) {
      fclose (fp);
      return H;
    }

    b = hash_add (H, s);
    b->f = 0.0;

    s = strtok (NULL, " \t");

    if (!s) {
      hash_delete (H, b->key);
      continue;
    }
    
    if (strcmp (s, "=") != 0) {
      hash_delete (H, b->key);
      continue;
    }

    s = strtok (NULL, " \t");

    if (!s) {
      hash_delete (H, b->key);
      continue;
    }
    
    if (sscanf (s, "%lg", &v) != 1) {
      hash_delete (H, b->key);
      continue;
    }
    b->f = v;
  }
  fclose (fp);
  
  return H;
}


#define CNLFP  _l->_line(); fprintf


Cell::Cell (Liberty *l, Process *p)
{
  _p = p;
  _l = l;
  a = ActNamespace::Act();
  _lfp = l->_lfp;
  A_INIT (_sh_vars);
  _num_inputs = 0;
  _num_outputs = 0;
  is_stateholding = NULL;
  _outvals = NULL;
  _tt[0] = NULL;
  _tt[1] = NULL;
  leakage_power = NULL;
  time_up = NULL;
  time_dn = NULL;
  is_out = NULL;
  st[0] = NULL;
  st[1] = NULL;
  A_INIT (dyn);

  
  ActPass *ap = a->pass_find ("prs2net");
  if (!ap) {
    fatal_error ("Need netlist!");
  }
  
  np = dynamic_cast<ActNetlistPass *> (ap);
  Assert (np, "What?");

  nl = np->getNL (p);

  if (!nl) {
    warning ("No netlist found for process `%s'", p->getName());
    return;
  }

  printf ("Cell: %s\n", p->getName());

  for (int i=0; i < A_LEN (nl->bN->ports); i++) {
    if (nl->bN->ports[i].omit) continue;
    if (nl->bN->ports[i].input) {
      _num_inputs++;
    }
    else {
      _num_outputs++;
    }
  }

  if (_num_inputs == 0 && _num_outputs == 0) {
    _is_dataflow = 1;
    /* -- check to see if this is a dataflow cell -- */
    for (int i=0; i < A_LEN (nl->bN->chpports); i++) {
      if (nl->bN->chpports[i].omit) continue;
      /* -- check if this is an int! */
      /* each channel is an input/output pair */
      _num_inputs++;
      _num_outputs++;
    }
  }
  else {
    _is_dataflow = 0;
  }
  
  if (_num_inputs == 0) {
    warning ("Cell %s: no inputs?", nl->bN->p->getName());
    nl = NULL;
    return;
  }
  if (_num_inputs > 8) {
    warning ("High fan-in cell?");
    nl = NULL;
    return;
  }
  if (_num_outputs == 0) {
    warning ("Cell %s: no outputs?", nl->bN->p->getName());
    nl = NULL;
    return;
  }

  if (_is_dataflow) {
    /* we're done! */
    return;
  }

  /* prepare for circuit characterization, in particular collect detailed
     information for state-holding cells.
  */
  for (node_t *n = nl->hd; n; n = n->next) {
    if (n->v) {
      if (n->v->e_up || n->v->e_dn) {
	/* there's a gate here */
	if ((n->v->stateholding && !n->v->unstaticized)
	    || n->v->manualkeeper != 0) {
	  if (!is_stateholding) {
	    is_stateholding = n;
	  }
	  else {
	    warning ("Analysis does not work for cells with multiple state-holding gates");
	    nl = NULL;
	    return;
	  }
	}
      }
    }
  }

  if (is_stateholding) {
    /* -- analyze state-holding operator -- */
    Assert (is_stateholding->v, "What?");
    A_INIT (_sh_vars);
    _collect_support (is_stateholding->v->e_up);
    _collect_support (is_stateholding->v->e_dn);
    
    Assert (A_LEN (_sh_vars) > 0, "What?!");

    _tt[0] = bitset_new (1 << A_LEN (_sh_vars));
    _tt[1] = bitset_new (1 << A_LEN (_sh_vars));
    bitset_clear (_tt[0]);
    bitset_clear (_tt[1]);
    
    _build_truth_table (is_stateholding->v->e_up, 1);
    _build_truth_table (is_stateholding->v->e_dn, 0);
  }
}

void Cell::_printHeader ()
{
  CNLFP (_lfp, "cell(");
  a->mfprintfproc (_lfp, _p);
  fprintf (_lfp, ") {\n");
  _l->_tab();

  /* XXX what is this? */
  CNLFP (_lfp, "area : 25;\n");

  /* XXX: fixme emit power and ground pins */
  CNLFP (_lfp, "pg_pin(GND) {\n");
  _l->_tab();
  CNLFP (_lfp, "pg_type : primary_ground;\n");
  CNLFP (_lfp, "voltage_name : GND;\n");
  _l->_untab();
  CNLFP (_lfp, "}\n");
    
  CNLFP (_lfp, "pg_pin(Vdd) {\n");
  _l->_tab();
  CNLFP (_lfp, "pg_type : primary_power;\n");
  CNLFP (_lfp, "voltage_name : Vdd;\n");
  _l->_untab();
  CNLFP (_lfp, "}\n");
}


void Cell::_printFooter ()
{
  _l->_untab();
  CNLFP (_lfp, "}\n");
}


void Cell::_add_support_var (ActId *id)
{
  act_connection *c;
  phash_bucket_t *b;

  c = id->Canonical (nl->bN->cur);
  for (int i=0; i < A_LEN (_sh_vars); i++) {
    if (c == _sh_vars[i]->id) return;
  }
  A_NEW (_sh_vars, act_booleanized_var_t *);

  b = phash_lookup (nl->bN->cH, c);
  Assert (b, "What?!");
  A_NEXT (_sh_vars) = (act_booleanized_var_t *)b->v;
  Assert (A_NEXT(_sh_vars)->id == c, "What?!");
  A_INC (_sh_vars);
}

void Cell::_collect_support (act_prs_expr_t *e)
{
  if (!e) return;
  switch (e->type) {
  case ACT_PRS_EXPR_AND:
  case ACT_PRS_EXPR_OR:
    _collect_support (e->u.e.l);
    _collect_support (e->u.e.r);
    break;

  case ACT_PRS_EXPR_NOT:
    _collect_support (e->u.e.l);
    break;

  case ACT_PRS_EXPR_LABEL:
    warning ("labels within state-holding gate...");
    break;

  case ACT_PRS_EXPR_TRUE:
  case ACT_PRS_EXPR_FALSE:
    break;

  case ACT_PRS_EXPR_VAR:
    _add_support_var (e->u.v.id);
    break;
    
  default:
    fatal_error ("Unexpected type %d", e->type);
    break;
  }
}

int Cell::_eval_expr (act_prs_expr_t *e, unsigned int v)
{
  act_connection *c;
  if (!e) return 0;
  switch (e->type) {
  case ACT_PRS_EXPR_AND:
    if (_eval_expr (e->u.e.l, v) && _eval_expr (e->u.e.r, v)) {
      return 1;
    }
    return 0;
    break;
  case ACT_PRS_EXPR_OR:
    if (_eval_expr (e->u.e.l, v) || _eval_expr (e->u.e.r, v)) {
      return 1;
    }
    return 0;
    break;

  case ACT_PRS_EXPR_NOT:
    return 1 - _eval_expr (e->u.e.l, v);
    break;

  case ACT_PRS_EXPR_LABEL:
    warning ("labels within state-holding gate...");
    return 0;
    break;

  case ACT_PRS_EXPR_TRUE:
    return 1;
    break;
    
  case ACT_PRS_EXPR_FALSE:
    return 0;
    break;

  case ACT_PRS_EXPR_VAR:
    c = e->u.v.id->Canonical (nl->bN->cur);
    for (int i=0; i < A_LEN (_sh_vars); i++) {
      if (_sh_vars[i]->id == c) {
	return ((v >> i) & 1);
      }
    }
    fatal_error ("Didn't find input variable?!");
    return 0;
    break;
    
  default:
    fatal_error ("Unexpected type %d", e->type);
    return 0;
    break;
  }
}

void Cell::_build_truth_table (act_prs_expr_t *e, int idx)
{
  for (unsigned int i=0; i < (1 << A_LEN (_sh_vars)); i++) {
    if (_eval_expr (e, i)) {
      bitset_set (_tt[idx], i);
    }
    else {
      bitset_clr (_tt[idx], i);
    }
  }
}


Cell::~Cell()
{
  if (_outvals) {
    for (int i=0; i < _num_outputs; i++) {
      bitset_free (_outvals[i]);
    }

    int x = _num_outputs;
  
    for (int i=0; i < A_LEN (_sh_vars); i++) {
      if (_sh_vars[i]->isport) continue;
      bitset_free (_outvals[x]);
      x++;
    }
    FREE (_outvals);
  }
  
  if (is_stateholding) {
    bitset_free (_tt[0]);
    bitset_free (_tt[1]);
    _tt[0] = NULL;
    _tt[1] = NULL;
  }
  A_FREE (_sh_vars);

  for (int i=0; i < A_LEN (dyn); i++) {
    FREE (dyn[i].delay);
    FREE (dyn[i].transit);
    FREE (dyn[i].intpow);
  }  

  A_FREE (dyn);

  if (time_up) {
    FREE (time_up);
  }
  if (time_dn) {
    FREE (time_dn);
  }
  if (is_out) {
    FREE (is_out);
  }
  if (st[0]) {
    bitset_free (st[0]);
  }
  if (st[1]) {
    bitset_free (st[1]);
  }
}

int Cell::_run_leakage ()
{
  FILE *sfp;
  char buf[1024];
  char file[1024];
  A_DECL (char *, outname);

  if (!nl) return 0;

  if (_is_dataflow) {
    return _run_dflow_leakage ();
  }

  A_INIT (outname);

  snprintf (file, 1024, "_splk_");
  a->msnprintfproc (file + 6, 1018, _p);

  snprintf (buf, 1024, "%s.spi", file);

  sfp = fopen (buf, "w");
  if (!sfp) {
    fatal_error ("Could not open `%s' for writing", buf);
  }

  /* -- std header that instantiates the module -- */
  
  if (!_gen_spice_header (sfp)) {
    fclose (sfp);
    unlink (buf);
    return 0;
  }

  /* -- generate all possible static input scenarios -- */
  _print_all_input_cases (sfp, "p");
  fprintf (sfp, "\n");

  double period = config_get_real ("xcell.period");
  double vdd = config_get_real ("xcell.Vdd");
  
  /* -- measurement of current -- */
  int tm = 1;
  double lk_window = config_get_real ("xcell.leak_window");
  if (period / (2 * lk_window) < 2) {
    fatal_error ("leak_window parameter (%g) is too large for period (%g)\n",
		 lk_window, period);
  }
  for (int i=0; i < (1 << _num_inputs); i++) {
    fprintf (sfp, ".measure tran current_%d avg i(Vv1) from %gp to %gp\n",
	     i, tm*period + lk_window, (tm+1)*period - lk_window);
    fprintf (sfp, ".measure tran leak_%d PARAM='-current_%d*%g'\n", i, i,
	     vdd);
    tm++;
  }

  fprintf (sfp, ".tran 0.1p %gp\n", tm*period);

  if (is_hspice()) {
    fprintf (sfp, ".options post post_version=9601\n");
    fprintf (sfp, ".options measform=2\n");
  }
  
  fprintf (sfp, ".print tran");
  if (is_xyce()) {
    fprintf (sfp, " format=raw");
  }
  
  /* print voltages */
  char bufout[1024];

  int num_outputs;
  for (int i=0; i < A_LEN (nl->bN->ports); i++) {
    if (nl->bN->ports[i].omit) continue;
    if (nl->bN->ports[i].input) continue;

    ActId *tmp;
    char buf[1024];
    tmp = nl->bN->ports[i].c->toid();
    tmp->sPrint (buf, 1024);
    delete tmp;
    fprintf (sfp, " V(xtst%s", config_get_string ("net.spice_path_sep"));
    a->mfprintf (sfp, "%s", buf);
    fprintf (sfp, ") ");

    snprintf (bufout, 1024, "xtst.");
    a->msnprintf (bufout + strlen (bufout), 1024 - strlen (bufout),
		  "%s", buf);
    for (int i=0; bufout[i]; i++) {
      bufout[i] = tolower(bufout[i]);
    }
    A_NEWM (outname, char *);
    A_NEXT (outname) = Strdup (bufout);
    A_INC (outname);
  }
  num_outputs = A_LEN (outname);
  Assert (_num_outputs == num_outputs, "What?");

  /*-- we also need internal nodes for stateholding gates --*/
  for (int i=0; i < A_LEN (_sh_vars); i++) {
    if (_sh_vars[i]->isport) continue;

    ActId *tmp;
    char buf[1024];
    tmp = _sh_vars[i]->id->toid();
    tmp->sPrint (buf, 1024);
    delete tmp;
    fprintf (sfp, " V(xtst%s", config_get_string ("net.spice_path_sep"));
    a->mfprintf (sfp, "%s", buf);
    fprintf (sfp, ") ");

    snprintf (bufout, 1024, "xtst.");
    a->msnprintf (bufout + strlen (bufout), 1024 - strlen (bufout),
		  "%s", buf);
    for (int i=0; bufout[i]; i++) {
      bufout[i] = tolower(bufout[i]);
    }
    A_NEWM (outname, char *);
    A_NEXT (outname) = Strdup (bufout);
    A_INC (outname);
  }
  
  fprintf (sfp, "\n");
	 
  fprintf (sfp, ".end\n");

  fclose (sfp);
  
  /* -- run the spice simulation -- */
  
  snprintf (buf, 1024, "%s %s.spi > %s.log 2>&1",
	    config_get_string ("xcell.spice_binary"),
	    file, file);

  system (buf);

  /* -- extract results from spice run -- */

  /* -- convert trace file to atrace format -- */
  if (config_get_int ("xcell.spice_output_fmt") == 0) {
    /* raw */
    snprintf (buf, 1024, "tr2alint -r %s.spi.raw %s", file, file);
  }
  else {
    snprintf (buf, 1024, "tr2alint %s.tr0 %s", file, file);
  }
  system (buf);

  /* 
     Step 1: truth tables
  */
  atrace *tr = atrace_open (file);
  if (!tr) {
    fatal_error ("Could not open simulation trace file!");
  }
  name_t *timenode;
  A_DECL (name_t *, outnode);
  A_INIT (outnode);
  
  snprintf (buf, 1024, "time");
  timenode = atrace_lookup (tr, buf);
  if (!timenode) {
    printf ("Time not found?\n");
  }

  for (int i=0; i < A_LEN (outname); i++) {
    A_NEWM (outnode, name_t *);
    A_NEXT (outnode) = atrace_lookup (tr, outname[i]);
    if (!A_NEXT (outnode)) {
      if (i < num_outputs) {
         snprintf (buf, 1024, "p%d", i + _num_inputs);
	 A_NEXT (outnode) = atrace_lookup (tr, buf);
      }
      if (!A_NEXT (outnode)) {
         printf ("Output node `%s' not found", outname[i]);
      }
      else {
         A_INC (outnode);
      }
    }
    else {
      A_INC (outnode);
    }
  }

  for (int i=0; i < A_LEN (outname); i++) {
    FREE (outname[i]);
  }
  
  if (A_LEN (outnode) != A_LEN (outname) || !timenode) {
    A_FREE (outnode);
    A_FREE (outname);
    atrace_close (tr);
    A_FREE (_sh_vars);
    return 0;
  }
  
  A_FREE (outname);

  int nnodes, nsteps, fmt, ts;
  if (atrace_header (tr, &ts, &nnodes, &nsteps, &fmt)) {
    fatal_error ("Trace file header corrupted?");
  }

  //printf ("%d nodes, %d steps\n", nnodes, nsteps);

  /* -- get values -- */
  tm = 1;
  atrace_init_time (tr);
  atrace_advance_time (tr, period*1e-12/ATRACE_GET_STEPSIZE (tr));

  float vhigh, vlow;

  vhigh = config_get_real ("lint.V_high");
  vlow = config_get_real ("lint.V_low");

  MALLOC (_outvals, bitset_t *, A_LEN (outnode));
  for (int i=0; i < A_LEN (outnode); i++) {
    _outvals[i] = bitset_new (1 << _num_inputs);
  }
  
  for (int i=0; i < (1 << _num_inputs); i++) {
    float val;
    
    atrace_advance_time (tr, (period-1000)*1e-12/ATRACE_GET_STEPSIZE (tr));

    for (int j=0; j < A_LEN (outnode); j++) {
      val = ATRACE_NODE_VAL (tr, outnode[j]);

      if (val >= vhigh) {
	bitset_set (_outvals[j], i);
	//printf ("out[%d]: H @ %g\n", j, ATRACE_NODE_VAL (tr, timenode));
      }
      else if (val <= vlow) {
	bitset_clr (_outvals[j], i);
	//printf ("out[%d]: L @ %g\n", j, ATRACE_NODE_VAL (tr, timenode));
      }
      else {
	if (j >= num_outputs) {
	  warning ("%s: out[%d]: X (%g) @ %g\n", _p->getName(), j, val, ATRACE_NODE_VAL (tr, timenode));
	}
      }
    }
    atrace_advance_time (tr, 1000e-12/ATRACE_GET_STEPSIZE (tr));
  }
  atrace_close (tr);

  /*
    Step 2: leakage measurements
  */
  snprintf (buf, 1024, "%s.spi.mt0", file);
  struct Hashtable *H = parse_measurements (buf);
  if (!H) {
    snprintf (buf, 1024, "%s.mt0", file);
    H = parse_measurements (buf);
    if (!H) {
      fatal_error ("Could not open measurement output file %s.\n", buf);
    }
  }

  MALLOC (leakage_power, double, (1 << _num_inputs));
  for (int i=0; i < (1 << _num_inputs); i++) {
    leakage_power[i] = 0;
  }

  hash_iter_t hi;
  hash_bucket_t *b;

  hash_iter_init (H, &hi);
  while ((b = hash_iter_next (H, &hi))) {
    int i;
    double lk;
    if (strncasecmp (b->key, "leak_", 5) == 0) {
      if (sscanf (b->key + 5, "%d", &i) != 1) {
	fatal_error ("Unknown measurement `%s'", b->key);
      }
      lk = b->f;

      if (lk < 0) {
	warning ("%s: measurement failed for leakage, scenario %d (%g)",
		 _p->getName(), i, lk);
      }
      leakage_power[i] = lk;
    }
  }
  hash_free (H);

  unlink_generic_trace (file);

  A_FREE (outnode);
  
  return 1;
}



int Cell::_gen_spice_header (FILE *fp)
{
  A_DECL (int, xout);
  A_INIT (xout);

  fprintf (fp, "**************************\n");
  fprintf (fp, "** characterization run **\n");
  fprintf (fp, "**************************\n");

  /*-- XXX: power and ground are actually in the netlist --*/

  fprintf (fp, ".include '%s'\n\n", config_get_string ("xcell.tech_setup"));
  fprintf (fp, ".global Vdd\n");
  fprintf (fp, ".global GND\n");

  if (is_xyce()) {
    fprintf (fp, ".global_param load = 2.2f\n");
  }
  else {
    fprintf (fp, ".param load = 2.2f\n");
  }
  fprintf (fp, ".param resistor = %g%s\n\n",
	   config_get_real ("xcell.R_value"),
	   config_get_string ("xcell.units.resis"));

  if (is_xyce()) {
    fprintf (fp, ".options DEVICE TNOM=%g\n", config_get_real ("xcell.T") - 273.15);
  }
  else {
    fprintf (fp, ".options TNOM=%g\n", config_get_real ("xcell.T") - 273);
  }

  /*-- dump subcircuit --*/
  np->Print (fp, _p);

  /*-- instantiate circuit --*/

  fprintf (fp, "\n\n");

  fprintf (fp, "xtst ");
  for (int i=0; i < A_LEN (nl->bN->ports); i++) {
    if (nl->bN->ports[i].omit) continue;
    if (!nl->bN->ports[i].input) {
      A_NEWM (xout, int);
      A_NEXT (xout) = i;
      A_INC (xout);
    }
    fprintf (fp, "p%d ", i);
  }
  a->mfprintfproc (fp, _p);
  fprintf (fp, "\n");

  if (A_LEN (xout) == 0) {
    warning ("Cell %s: no outputs?\n", _p->getName());
    return 0;
  }

  /*-- power supplies --*/
  fprintf (fp, "Vv0 GND 0 0.0\n");
  fprintf (fp, "Vv1 Vdd 0 %g\n", config_get_real ("xcell.Vdd"));

  /*-- load cap on output that is swept --*/
  for (int i=0; i < A_LEN (xout); i++) {
    fprintf (fp, "Clc%d p%d GND load\n\n", i, xout[i]);
  }
  A_FREE (xout);

  return 1;
}


int Cell::_get_input_pin (int pin)
{
  int pos = pin;
  int j;

  if (_is_dataflow) {
    for (j=0; j < A_LEN (nl->bN->chpports); j++) {
      if (nl->bN->chpports[j].omit) continue;
      if (pos == 0) {
	return j;
      }
      pos--;
    }
  }
  else {
    for (j=0; j < A_LEN (nl->bN->ports); j++) {
      if (nl->bN->ports[j].omit) continue;
      if (!nl->bN->ports[j].input) continue;
      if (pos == 0) {
	return j;
      }
      pos--;
    }
  }
  Assert (0, "Could not find input pin!");
  return -1;
}

int Cell::_get_output_pin (int pin)
{
  int pos = pin;
  int j;

  if (_is_dataflow) {
    for (j=0; j < A_LEN (nl->bN->chpports); j++) {
      if (nl->bN->chpports[j].omit) continue;
      if (pos == 0) {
	return j;
      }
      pos--;
    }
  }
  else {
    for (j=0; j < A_LEN (nl->bN->ports); j++) {
      if (nl->bN->ports[j].omit) continue;
      if (nl->bN->ports[j].input) continue;
      if (pos == 0) {
	return j;
      }
      pos--;
    }
  }
  Assert (0, "Could not find output pin!");
  return -1;
}


void Cell::_print_all_input_cases (FILE *sfp, const char *prefix)
{
  double vdd = config_get_real ("xcell.Vdd");
  double period = config_get_real ("xcell.period");

  /* -- leakage scenarios -- */
  
  for (int k=0; k < _num_inputs; k++) {
    fprintf (sfp, "Vn%d %s%d 0 PWL (0p 0 1000p 0\n",
	     _get_input_pin (k), prefix, _get_input_pin (k));
    int tm = 1;
    for (int i=0; i < (1 << _num_inputs); i++) {
      if ((i >> k) & 0x1) {
	fprintf (sfp, "+%gp %g %gp %g\n", tm*period + 1, vdd, (tm+1)*period, vdd);
      }
      else {
	fprintf (sfp, "+%gp 0 %gp 0\n", tm*period + 1, (tm+1)*period);
      }
      tm++;
    }
    fprintf (sfp, "+)\n\n");
  }
}


void Cell::_emit_leakage ()
{
  int i;
  char buf[1024];

  if (!nl) return;

  for (int i=0; i < (1 << _num_inputs); i++) {
    double lk = leakage_power[i];

    if (!_is_dataflow) {
      /*-- now check for interference --*/

      /*--
	Primary inputs are set by "i"
	Secondary inputs are set by bitset outval[...]
	--*/

      unsigned int val = 0;
      int loc = _num_outputs;
      
      /* -- compute variable assignment -- */
      for (int k=0; k < A_LEN (_sh_vars); k++) {
	/* search in port list */
	if (_sh_vars[k]->isport) {
	  int j;
	  int pos = 0;
	  int opos = 0;
	  for (j=0; j < A_LEN (nl->bN->ports); j++) {
	    if (nl->bN->ports[j].omit) continue;
	    if (nl->bN->ports[j].c == _sh_vars[k]->id) {
	      break;
	    }
	    if (nl->bN->ports[j].input) {
	      pos++;
	    }
	    else {
	      opos++;
	    }
	  }
	  Assert (j != A_LEN (nl->bN->ports), "What?!");

	  if (nl->bN->ports[j].input) {
	    if ((i >> pos) & 1) {
	      val = val | (1 << k);
	    }
	  }
	  else {
	    /* it's an output! */
	    if (bitset_tst (_outvals[opos], i)) {
	      val = val | (1 << k);
	    }
	  }
	}
	else {
	  if (bitset_tst (_outvals[loc], i)) {
	    val = val | (1 << k);
	  }
	  loc++;
	}
      }

      if (is_stateholding) {
	if (bitset_tst (_tt[0], val) && bitset_tst (_tt[1], val)) {
	  /* -- interference -- */
	  continue;
	}
      }
    }
      
    CNLFP (_lfp, "leakage_power() {\n");
    _l->_tab();
    CNLFP (_lfp, "when : \"");
    for (int k=0; k < _num_inputs; k++) {
      _sprint_input_pin (buf, 1024, k);
      if (k > 0) {
	fprintf (_lfp, "&");
      }
      if (((i >> k) & 1) == 0) {
	fprintf (_lfp, "!");
      }
      a->mfprintf (_lfp, "%s", buf);
    }
    fprintf (_lfp, "\";\n");
    CNLFP (_lfp, "value : %g;\n", lk/config_get_real ("xcell.units.power_conv"));
    _l->_untab();
    CNLFP (_lfp, "}\n");
  }
}



int Cell::_run_input_cap ()
{
  FILE *sfp;
  char buf[1024];
  char file[1024];

  if (!nl) {
    return 0;
  }

  if (_is_dataflow) {
    return _run_dflow_input_cap ();
  }

  snprintf (file, 1024, "_spcp_");
  a->msnprintfproc (file + 6, 1018, _p);

  snprintf (buf, 1024, "%s.spi", file);
  sfp = fopen (buf, "w");
  if (!sfp) {
    fatal_error ("Could not open `%s' for writing", buf);
  }
  if (!_gen_spice_header (sfp)) {
    fclose (sfp);
    unlink (buf);
    return 0;
  }

  /* -- resis to input -- */

  int pos = 0;
  for (int i=0; i < A_LEN (nl->bN->ports); i++) {
    if (nl->bN->ports[i].omit) continue;
    if (!nl->bN->ports[i].input) continue;
    fprintf (sfp, "Rdrv%d q%d p%d resistor\n", pos, pos, pos);
    pos++;
  }
  fprintf (sfp, "\n");

  _print_input_cap_cases (sfp, "q");

  /* measure input delays! */

  double period = config_get_real ("xcell.period");
  double vdd = config_get_real ("xcell.Vdd");
  double window = config_get_real ("xcell.short_window");
    
  for (int i=0; i < _num_inputs; i++) {
    for (int j=0; j < ((1 << (_num_inputs-1))); j++) {
      double my_start = i*(1 << (_num_inputs-1))*period + period + period*j;
      
      fprintf (sfp, ".measure tran cap_tup_%d_%d_0 trig V(q%d) VAL=%g TD=%gp RISE=1 TARG V(p%d) VAL=%g\n", i, j, i, (vdd*0.05),  my_start + window,
	       i, vdd*(1-config_get_real ("xcell.cap_measure")));
      fprintf (sfp, ".measure tran cap_tup_%d_%d_1 trig V(q%d) VAL=%g TD=%gp RISE=2 TARG V(p%d) VAL=%g\n", i, j, i, (vdd*0.05),
	       my_start + window,
	       i, vdd*(1-config_get_real ("xcell.cap_measure")));
      fprintf (sfp, ".measure tran cap_tdn_%d_%d_0 trig V(q%d) VAL=%g TD=%gp FALL=1 TARG V(p%d) VAL=%g\n", i, j, i, (vdd*0.95),
	       my_start + window,
	       i, vdd*(1-config_get_real ("xcell.cap_measure")));
      fprintf (sfp, ".measure tran cap_tdn_%d_%d_1 trig V(q%d) VAL=%g TD=%gp FALL=2 TARG V(p%d) VAL=%g\n", i, j, i, (vdd*0.95),
	       my_start + window,
	       i, vdd*(1-config_get_real ("xcell.cap_measure")));
    }
  }

  fprintf (sfp, ".tran 0.1p %gp\n",
	   _num_inputs * ((1 << (_num_inputs-1))) * period + period);
  
  if (is_hspice()) {
    fprintf (sfp, ".options measform=2\n");
  }
  fprintf (sfp, "\n.end\n");
  fclose (sfp);

  snprintf (buf, 1024, "%s %s.spi > %s.log 2>&1",
	    config_get_string ("xcell.spice_binary"), file, file);
  system (buf);


  int *upcnt, *dncnt;
  MALLOC (upcnt, int, _num_inputs);
  MALLOC (dncnt, int, _num_inputs);
  MALLOC (time_up, double, _num_inputs);
  MALLOC (time_dn, double, _num_inputs);
  for (int i=0; i < _num_inputs; i++) {
    time_up[i] = 0;
    time_dn[i] = 0;
    upcnt[i] = 0;
    dncnt[i] = 0;
  }

  snprintf (buf, 1024, "%s.spi.mt0", file);
  struct Hashtable *H = parse_measurements (buf);
  if (!H) {
    snprintf (buf, 1024, "%s.mt0", file);
    H = parse_measurements (buf);
    if (!H) {
      fatal_error ("Could not open measurement output from simulation.\n");
    }
  }

  hash_iter_t hi;
  hash_bucket_t *b;

  hash_iter_init (H, &hi);

  while ((b = hash_iter_next (H, &hi))) {
    int i, j;
    double tm;
    if (strncasecmp (b->key, "cap_tup_", 8) == 0) {
      if (sscanf (b->key + 8, "%d_%d", &i, &j) != 2) {
	fatal_error ("Unknown measurement: %s", b->key);
      }
      tm = b->f;
      if (tm < 0) {
	warning ("%s: negative input cap measurement %d_%d",
		 _p->getName(), i, j);
      }
      time_up[i] += tm;
      upcnt[i]++;
    }
    else if (strncasecmp (b->key, "cap_tdn_", 8) == 0) {
      if (sscanf (b->key + 8, "%d_%d", &i, &j) != 2) {
	fatal_error ("Unknown measurement: %s", b->key);
      }
      tm = b->f;
      if (tm < 0) {
	warning ("%s: negative input cap measurement %d_%d",
		 _p->getName(), i, j);
      }
      time_dn[i] += tm;
      dncnt[i]++;
    }
  }
  hash_free (H);

  for (int i=0; i < _num_inputs; i++) {
    if (upcnt[i] != dncnt[i]) {
      warning ("What?!");
    }
    if (i > 0 && upcnt[i] != upcnt[i-1]) {
      warning ("What?!!");
    }
    if (upcnt[i] == 0 || dncnt[i] == 0) {
      warning ("Bad news: no counts for scenario %d", i);
      continue;
    }
    time_up[i] /= upcnt[i];
    time_dn[i] /= dncnt[i];

    time_up[i] = time_up[i]/(log(1/config_get_real ("xcell.cap_measure"))*
			     config_get_real ("xcell.R_value")*
			     config_get_real ("xcell.units.resis_conv"));

    time_dn[i] = time_dn[i]/(log(1/config_get_real ("xcell.cap_measure"))*
			     config_get_real ("xcell.R_value")*
			     config_get_real ("xcell.units.resis_conv"));
  }
  FREE (upcnt);
  FREE (dncnt);

  unlink_generic (file);
  
  return 1;
}

void Cell::_emit_input_cap ()
{
  char buf[1024];
  
  if (!nl) return;
  
  for (int i=0; i < _num_inputs; i++) {
    _sprint_input_pin (buf, 1024, i);
    CNLFP (_lfp, "pin(");
    a->mfprintf (_lfp, "%s", buf);  fprintf (_lfp, ") {\n");
    _l->_tab();
    CNLFP (_lfp, "direction : input;\n");
    CNLFP (_lfp, "rise_capacitance : %g;\n", time_up[i]/config_get_real ("xcell.units.cap_conv"));
    CNLFP (_lfp, "fall_capacitance : %g;\n", time_dn[i]/config_get_real ("xcell.units.cap_conv"));
    _l->_untab();
    CNLFP (_lfp, "}\n");
  }
}


void Cell::_print_input_cap_cases (FILE *sfp, const char *prefix)
{
  double vdd = config_get_real ("xcell.Vdd");
  double period = config_get_real ("xcell.period");
  double window = config_get_real ("xcell.short_window");

  for (int i=0; i < _num_inputs; i++) {

    fprintf (sfp, "Vn%d %s%d 0 PWL (0p 0 1000p 0\n",
	     _get_input_pin (i), prefix, _get_input_pin (i));
    
    /*-- we run input i up and down 2 times, with the others being in
      all possible different states --*/
    int tm = 1;

    /* prefix: just go through all possible cases */
    for (int p=0; p < i; p++) {
      for (int q=0; q < (1 << (_num_inputs-1)); q++) {
	if ((q >> (i-1)) & 1) {
	  fprintf (sfp, "+%gp %g %gp %g\n", tm*period + 1, vdd, (tm+1)*period, vdd);
	}
	else {
	  fprintf (sfp, "+%gp 0 %gp 0\n", tm*period + 1, (tm+1)*period);
	}
	tm++;
      }
    }

    /* -- now it is my turn -- */
    for (int q=0; q < (1 << (_num_inputs-1)); q++) {
      double offset = tm*period;
      fprintf (sfp, "+%gp %g %gp %g\n", offset + 1, 0.0, offset + window, 0.0);
      offset += window;
      fprintf (sfp, "+%gp %g %gp %g\n", offset + 1, vdd, offset + window, vdd);
      offset += window;
      fprintf (sfp, "+%gp %g %gp %g\n", offset + 1, 0.0, offset + window, 0.0);
      offset += window;
      fprintf (sfp, "+%gp %g %gp %g\n", offset + 1, vdd, offset + window, vdd);
      offset += window;
      fprintf (sfp, "+%gp %g %gp %g\n", offset + 1, 0.0, offset + window, 0.0);
      offset += window;
      tm++;
      if (offset >= tm*period) {
	warning ("Period (%g) needs to be >= short_window (%g)*5\n", period, window);
      }
    }

    /* -- now rest -- */
    for (int p=i+1; p < _num_inputs; p++) {
      for (int q=0; q < (1 << (_num_inputs-1)); q++) {
	if ((q >> i) & 1) {
	  fprintf (sfp, "+%gp %g %gp %g\n", tm*period + 1, vdd, (tm+1)*period, vdd);
	}
	else {
	  fprintf (sfp, "+%gp 0 %gp 0\n", tm*period + 1, (tm+1)*period);
	}
	tm++;
      }
    }
  }
}


void Cell::_print_input_case (int idx, int skipmask)
{
  char buf[1024];
  ActId *tmp;
  int first = 1;
  
  for (int j=0; j < _num_inputs; j++) {
    if (skipmask & (1 << j)) continue;
    
    if (!first) {
      fprintf (_lfp, "*");
    }
    first = 0;
	    
    if (!((idx >> j) & 1)) {
      fprintf (_lfp, "!");
    }
    _sprint_input_pin (buf, 1024, j);
    a->mfprintf (_lfp, "%s", buf);
  }
}


static int find_driven_assignment (bitset_t *b, bitset_t *bopp,
				   int ninputs, int bit_pos, int bit_val,
				   int base_idx)
{
  int idx;
  int i;

  i = 0;

  for (i = 0; i < (1 << (ninputs-1)); i++) {
    idx = (((unsigned)i >> (bit_pos)) << (bit_pos+1)) |
      (bit_val << bit_pos) | (i & ((1 << bit_pos)-1));

    if (bitset_tst (b, idx) && !bitset_tst (bopp, idx)) {
      break;
    }
  }

  if (i == (1 << (ninputs-1))) {
    return -1;
  }

  idx = idx ^ base_idx;

  int cur_idx;

  cur_idx = base_idx;

  for (int i=0; i < ninputs; i++) {
    if (idx & (1 << i)) {
      cur_idx = cur_idx ^ (1 << i);
      if (bitset_tst (b, cur_idx) && !bitset_tst (bopp, cur_idx)) {
	return cur_idx;
      }
      else if (bitset_tst (bopp, cur_idx)) {
	return idx ^ base_idx;
      }
    }
  }
  return idx ^ base_idx;
}


/*
 * We know that the first serach failed
 */
static int find_multi_driven_assignment (bitset_t *b, bitset_t *bopp,
					 int ninputs, int bit_pos, int bit_val,
					 int base_idx)
{
  int idx;
  int i;

  i = 0;

  for (i = 0; i < (1 << (ninputs-1)); i++) {
    idx = (((unsigned)i >> (bit_pos)) << (bit_pos+1)) |
      ((1-bit_val) << bit_pos) | (i & ((1 << bit_pos)-1));

    if (bitset_tst (b, idx) && !bitset_tst (bopp, idx)) {
      break;
    }
  }

  if (i == (1 << (ninputs-1))) {
    return -1;
  }

  idx = idx ^ base_idx;

  int cur_idx;

  cur_idx = base_idx;

  for (int i=0; i < ninputs; i++) {
    if (idx & (1 << i)) {
      cur_idx = cur_idx ^ (1 << i);
      if (bitset_tst (b, cur_idx) && !bitset_tst (bopp, cur_idx)) {
	return cur_idx;
      }
      else if (bitset_tst (bopp, cur_idx)) {
	return idx ^ base_idx;
      }
    }
  }
  return idx ^ base_idx;
}


void Cell::_calc_dynamic ()
{
  int sh_outvar;

  if (_is_dataflow) {
    


    
    return;
  }

  if (A_LEN (dyn) > 0) {
    /* already computed! */
    return;
  }

  A_INIT (dyn);
  
  if (!nl) {
    return;
  }

  /* Run dynamic scenarios.

     If there's a state-holding node, build state-holding truth table
     from the existing truth tables.

     The assumption is that the state-holding gate is fully controlled
     from the primary input.
  */
  if (is_stateholding) {
    int *iomap;
    int *xmap;
    int xcount;
    
    /* -- we know the truth table for the pull-up and pull-down, so
          create it based directly on input 
     --*/
    st[0] = bitset_new (_num_inputs); /* pull-down */
    bitset_clear (st[0]);
    st[1] = bitset_new (_num_inputs); /* pull-up */
    bitset_clear (st[1]);

    MALLOC (iomap, int, _num_inputs + _num_outputs);
    for (int i=0; i < _num_inputs + _num_outputs; i++) {
      iomap[i] = -1;
    }

    xcount = 0;
    for (int i=0; i < A_LEN (_sh_vars); i++) {
      if (_sh_vars[i]->isport) continue;
      xcount++;
    }

    if (xcount > 0) {
      MALLOC (xmap, int, xcount);
      for (int i=0; i < xcount; i++) {
	xmap[i] = -1;
      }
    }

    /* build map from sh_vars to inputs */
    for (int i=0; i < A_LEN (_sh_vars); i++) {
      int pos = 0;
      int opos = _num_inputs;
      int xpos = 0;
      for (int j=0; j < A_LEN (nl->bN->ports); j++) {
	if (nl->bN->ports[j].omit) continue;
	if (nl->bN->ports[j].c == _sh_vars[i]->id) {
	  if (nl->bN->ports[j].input) {
	    iomap[pos] = i;
	  }
	  else {
	    iomap[opos] = i;
	  }
	  break;
	}
	if (nl->bN->ports[j].input) {
	  pos++;
	}
	else {
	  opos++;
	}
      }
      if (pos >= _num_inputs && opos >= (_num_inputs + _num_outputs)) {
	if (_sh_vars[i]->isport) {
	  fatal_error ("Map error?");
	}
	else {
	  Assert (xpos < xcount, "Hmmm");
	  xmap[xpos] = i;
	  xpos++;
	}
      }
    }

    for (int i=0; i < (1 << _num_inputs); i++) {
      unsigned int idx = 0;

      for (int j=0; j < _num_inputs; j++) {
	if (iomap[j] != -1) {
	  if ((i >> j) & 0x1) {
	    idx |= (1 << iomap[j]);
	  }
	}
      }

      for (int j=0; j < _num_outputs; j++) {
	if (iomap[_num_inputs + j] != -1) {
	  if (bitset_tst (_outvals[j], i)) {
	    idx |= (1 << iomap[_num_inputs + j]);
	  }
	}
      }

      for (int j=0; j < xcount; j++) {
	if (xmap[j] != -1) {
	  if (bitset_tst (_outvals[j + _num_outputs], i)) {
	    idx |= (1 << xmap[j]);
	  }
	}
      }

      if (bitset_tst (_tt[0], idx)) {
	bitset_set (st[0], i);
      }
      if (bitset_tst (_tt[1], idx)) {
	bitset_set (st[1], i);
      }
    }
    if (xcount > 0) {
      FREE (xmap);
    }
    FREE (iomap);


    /* -- 
       if sh is not a port, then check if there is an output that's
       exactly the complement of this variable
       -- */

    MALLOC (is_out, int, _num_outputs);
    for (int i=0; i < _num_outputs; i++) {
      is_out[i] = -1;
    }
    /* -1 = new, -2 = no, 0 = non-inv, 1 = inv */

    for (int i=0; i < (1 << _num_inputs); i++) {
      if (bitset_tst (st[0], i)) {
	/* pull-down is 1 */
	for (int j=0; j < _num_outputs; j++) {
	  if (is_out[j] == -2) continue;
	  if (bitset_tst (_outvals[j], i)) {
	    if (is_out[j] == -1) {
	      is_out[j] = 1;
	    }
	    else if (is_out[j] != 1) {
	      is_out[j] = -2;
	    }
	  }
	  else {
	    if (is_out[j] == -1) {
	      is_out[j] = 0;
	    }
	    else if (is_out[j] != 0) {
	      is_out[j] = -2;
	    }
	  }
	}
      }
      else if (bitset_tst (st[1], i)) {
	/* pull-up is 1 */
	for (int j=0; j < _num_outputs; j++) {
	  if (is_out[j] == -2) continue;
	  if (!bitset_tst (_outvals[j], i)) {
	    if (is_out[j] == -1) {
	      is_out[j] = 1;
	    }
	    else if (is_out[j] != 1) {
	      is_out[j] = -2;
	    }
	  }
	  else {
	    if (is_out[j] == -1) {
	      is_out[j] = 0;
	    }
	    else if (is_out[j] != 0) {
	      is_out[j] = -2;
	    }
	  }
	}
      }
    }

    sh_outvar = -1;
    for (int j=0; j < _num_outputs; j++) {
      if (is_out[j] == 0) {
	sh_outvar = 2*j;
	break;
      }
      if (is_out[j] == 1) {
	sh_outvar = 2*j+1;
	break;
      }
    }

    if (sh_outvar == -1) {
      warning ("Couldn't find output that corresponds to state-holding gate");
    }
  }

  /* -- create dynamic scenarios -- */
  
  for (int nout=0; nout < _num_outputs; nout++) {
    int pos = _get_output_pin (nout);

    for (int i=0; i < (1 << _num_inputs); i++) {
      /* -- current input vector scenario: i -- */
      if (is_stateholding && (is_out[nout] == 0 || is_out[nout] == 1)) {
	int drive_up = -1;
	/*-- this output is the state-holding gate --*/
	if (bitset_tst (st[1], i) && !bitset_tst (st[0], i)) {
	  drive_up = 1;
	}
	else if (bitset_tst (st[0], i) && !bitset_tst (st[1], i)) {
	  drive_up = 0;
	}

	if (drive_up != -1) {
	  /* state-holding output driven high */
#if 0	  
	  printf ("find-sh-case (%d): ", drive_up);
	  _print_input_case (stdout, a, nl, num_inputs, i);
	  printf ("\n");
#endif	  

	  for (int j=0; j < _num_inputs; j++) {
	    unsigned int opp = i ^ (1 << j);

	    /* flip bit j: if this turns the gate SH or opp driven,
	       we have a scenario */
	    if (!bitset_tst (st[drive_up], opp)) {
	      if (bitset_tst (st[1-drive_up], opp)) {

		A_NEW (dyn, struct dynamic_case);
		A_NEXT (dyn).nidx = 2;
		A_NEXT (dyn).idx[0] = opp;
		A_NEXT (dyn).idx[1] = i;
		A_NEXT (dyn).out_id = nout;
		A_NEXT (dyn).in_id = j;
		A_NEXT (dyn).in_init = ((opp >> j) & 1);
		A_NEXT (dyn).out_init = (drive_up ? 0 : 1) ^ is_out[nout];
		A_INC (dyn);
		
		//printf (" flip %d (comb)\n", j);
	      }
	      else {
		int k = find_driven_assignment (st[1-drive_up], st[drive_up],
						_num_inputs, j, (1-((i >> j) & 1)),
						opp);
		if (k >= 0) {
		  A_NEW (dyn, struct dynamic_case);
		  A_NEXT (dyn).nidx = 3;
		  A_NEXT (dyn).idx[0] = k;
		  A_NEXT (dyn).idx[1] = opp;
		  A_NEXT (dyn).idx[2] = i;
		  A_NEXT (dyn).out_id = nout;
		  A_NEXT (dyn).in_id = j;
		  A_NEXT (dyn).in_init = ((opp >> j) & 1);
		  A_NEXT (dyn).out_init = (drive_up ? 0 : 1) ^ is_out[nout];
		  A_INC (dyn);
		
		  //printf (" flip %d (sh) ... from ", j);
		  //_print_input_case (stdout, a, nl, num_inputs, k);
		  //printf (" to ");
		  //_print_input_case (stdout, a, nl, num_inputs, opp);
		  //printf ("\n");
		}
		else {
		  k = find_multi_driven_assignment (st[1-drive_up], st[drive_up],
						    _num_inputs, j, (1-((i>>j)&1)), opp);
		  if (k < 0) {
		    //printf (" flip %d (sh) not found\n", j);
		  }
		  else {
		    A_NEW (dyn, struct dynamic_case);
		    A_NEXT (dyn).nidx = 4;
		    A_NEXT (dyn).idx[0] = k;
		    A_NEXT (dyn).idx[1] = k ^ (1 << j);
		    A_NEXT (dyn).idx[2] = opp;
		    A_NEXT (dyn).idx[3] = i;
		    A_NEXT (dyn).out_id = nout;
		    A_NEXT (dyn).in_id = j;
		    A_NEXT (dyn).in_init = ((opp >> j) & 1);
		    A_NEXT (dyn).out_init = (drive_up ? 0 : 1) ^ is_out[nout];
		    A_INC (dyn);
		    
		    //printf (" flip %d (sh) ... from ", j);
		    //_print_input_case (stdout, a, nl, num_inputs, k);
		    //printf (" to ");
		    //_print_input_case (stdout, a, nl, num_inputs,
		    //k ^ (1 << j));
		    //printf (" to ");
		    //_print_input_case (stdout, a, nl, num_inputs, opp);
		    //printf ("\n");
		  }
		}
	      }
	    }
	  }
	}
      }
      else {
	if (is_stateholding) {
	  warning ("We assume the other outputs are combinational!");
	}
	
	/*-- combinational logic --*/
	  
	for (int j=0; j < _num_inputs; j++) {
	  /* -- current bit being checked: j -- */
	  unsigned int opp = i ^ (1 << j);

	  /* _outvals has truth table for output */
	  if ((!!bitset_tst (_outvals[nout], i)) !=
	      (!!bitset_tst (_outvals[nout], opp))) {
	    /* Found transition! Characterize this transition */
	    A_NEW (dyn, struct dynamic_case);
	    A_NEXT (dyn).nidx = 2;
	    A_NEXT (dyn).idx[0] = opp;
	    A_NEXT (dyn).idx[1] = i;
	    A_NEXT (dyn).out_id = nout;
	    A_NEXT (dyn).in_id = j;
	    A_NEXT (dyn).in_init = ((opp >> j) & 1);
	    A_NEXT (dyn).out_init = !!bitset_tst (_outvals[nout], opp);
	    A_INC (dyn);
#if 0	    
	    printf ("input: %d -> %d : ", (opp >> j) & 1, (i >> j) & 1);
	    _print_input_case (stdout, a, nl, num_inputs, i);
	    printf ("; out: %d -> %d\n", !!bitset_tst (_outvals[nout], opp),
		    !!bitset_tst (_outvals[nout], i));
#endif	    
	  }
	}
      }
    }
  }

#if 0
  for (int i=0; i < A_LEN (dyn); i++) {
    printf ("%d-step: (out[%d]: %s; in[%d]: %s)", dyn[i].nidx,
	    dyn[i].out_id, dyn[i].out_init ? "fall" : "rise",
	    dyn[i].in_id, dyn[i].in_init ? "fall" : "rise");
    printf ("\n  ");
    for (int j=0; j < dyn[i].nidx; j++) {
      if (j != 0) {
	printf (" -> ");
      }
      _print_input_case (stdout, a, nl, num_inputs, dyn[i].idx[j]);
    }
    printf ("\n");
  }
#endif
}
  
/*------------------------------------------------------------------------
 *
 * Dynamic scenarios
 *
 *------------------------------------------------------------------------
 */
int Cell::_run_dynamic ()
{
  FILE *sfp;
  char buf[1024];
  char file[1024];

  if (!nl) {
    return 0;
  }
  if (_is_dataflow) {
    return _run_dflow_dynamic ();
  }
  
  _calc_dynamic ();
    
  /* -- create spice file -- */

  snprintf (file, 1024, "_spdy_");
  a->msnprintfproc (file + 6, 1018, _p);
  
  snprintf (buf, 1024, "%s.spi", file);
  sfp = fopen (buf, "w");
  if (!sfp) {
    fatal_error ("Could not open `%s' for writing", buf);
  }

  /* -- std header that instantiates the module -- */
  if (!_gen_spice_header (sfp)) {
    fclose (sfp);
    unlink (buf);
    return 0;
  }

  /* emit waveform for each input */
  double window = config_get_real ("xcell.short_window");
  double vdd = config_get_real ("xcell.Vdd");
  double period = config_get_real ("xcell.period");
  int nslew = config_get_table_size ("xcell.input_trans");
  double *slew_table = config_get_table_real ("xcell.input_trans");
  int tm;

  if (A_LEN (dyn) == 0) {
    warning ("Cell characterization failed; no arcs detected?");
    fclose (sfp);
    snprintf (buf, 1024, "%s.spi", file);
    unlink (buf);
    return 0;
  }

  for (int i=0; i < _num_inputs; i++) {
    fprintf (sfp, "Vn%d p%d 0 PWL (0p 0 1000p %g\n", _get_input_pin (i),
	     _get_input_pin (i),
	     ((dyn[0].idx[0] >> i) & 1) ? vdd : 0.0);

    tm = 1;
    /*-- this has to be done with different input slew --*/
    for (int ns=0; ns < nslew; ns++) {
      for (int j=0; j < A_LEN (dyn); j++) {
	for (int k=0; k < dyn[j].nidx; k++) {
	  double val = ((dyn[j].idx[k] >> i) & 1) ? vdd : 0.0;

	  if (k != (dyn[j].nidx-1) || i != (dyn[j].in_id)) {
	    fprintf (sfp, "+%gp %g %gp %g\n", tm*period +
		     0.25*window + k*window, val,
		     tm*period + (k+1)*window, val);
	  }
	  else {
	    double correction = 0.0;
	    if (dyn[j].in_init == 0) {
	      correction = (config_get_real ("xcell.waveform.rise_high") -
			    config_get_real ("xcell.waveform.rise_low"))/100.0;
	    }
	    else {
	      correction = (config_get_real ("xcell.waveform.fall_high") -
			    config_get_real ("xcell.waveform.fall_low"))/100.0;
	    }
	    fprintf (sfp, "+%gp %g %gp %g\n", tm*period + k*window + slew_table[ns]/correction, val, tm*period + (k+1)*window, val);

	    if (slew_table[ns]/correction >= window) {
	      warning ("Window is too small; needs to be at least %g\n",
		       slew_table[ns]/correction);
	    }
	  }
	}
	tm++;
      }
    }
    fprintf (sfp, "\n");
  }

  if (is_xyce()) {
    fprintf (sfp, "\n.tran 0.1p %gp\n", tm*period);
#if 0
    fprintf (sfp, "\n.print tran");
    for (int i=0; i < num_inputs + _num_outputs; i++) {
      fprintf (sfp, " V(p%d)", i);
    }
    fprintf (sfp, "\n");
#endif
    /* -- sweep load! -- */
    fprintf (sfp, "\n.step load LIST ");
    double *load_table = config_get_table_real ("xcell.load");
    for (int i=0; i < config_get_table_size ("xcell.load"); i++) {
      fprintf (sfp, " %gf", load_table[i]);
    }
    fprintf (sfp, "\n\n");
  }
  else if (is_hspice()) {
    fprintf (sfp, "\n.tran 0.1p %gp ", tm*period);

    /* -- sweep load! -- */
    fprintf (sfp, "SWEEP load POI %d", config_get_table_size ("xcell.load"));
    double *load_table = config_get_table_real ("xcell.load");
    for (int i=0; i < config_get_table_size ("xcell.load"); i++) {
      fprintf (sfp, " %gf", load_table[i]);
    }
    fprintf (sfp, "\n\n");
    fprintf (sfp, ".options measform=2\n");
  }
  else {
    fatal_error ("What?");
  }

  /*-- allocate space for dynamic measurements --*/

  int nsweep = config_get_table_size ("xcell.load");

  for (int i=0; i < A_LEN (dyn); i++) {
    MALLOC (dyn[i].delay, double, nsweep*nslew);
    MALLOC (dyn[i].transit, double, nsweep*nslew);
    MALLOC (dyn[i].intpow, double, nsweep*nslew);
    for (int j=0; j < nsweep*nslew; j++) {
      dyn[i].delay[j] = 0;
      dyn[i].transit[j] = 0;
      dyn[i].intpow[j] = 0;
    }
  }

  /* measure output transit time and delay */
    
  /*-- this has to be done with different input slew --*/
  tm = 1;
  for (int ns=0; ns < nslew; ns++) {
    for (int j=0; j < A_LEN (dyn); j++) {
      int k = dyn[j].nidx-1;
      double off = tm*period + k*window;
      double st, end;

      /* 
	 1. measure delay : 50% input to 50% output 
      */
      fprintf (sfp, ".measure tran delay_%d_%d trig V(p%d) VAL=%g TD=%gp CROSS=1 targ V(p%d) VAL=%g\n", j, ns, _get_input_pin (dyn[j].in_id), vdd*0.5,
	       off, _get_output_pin (dyn[j].out_id), vdd*0.5);

      fprintf (sfp, ".measure tran negdelay_%d_%d trig V(p%d) VAL=%g TD=%gp CROSS=1 targ V(p%d) VAL=%g\n", j, ns, _get_output_pin (dyn[j].out_id), vdd*0.5,
	       off, _get_input_pin (dyn[j].in_id), vdd*0.5);
      /*
	2. measure output transit time
      */
      if (dyn[j].out_init == 0) {
	st = vdd*config_get_real ("xcell.waveform.rise_low")/100.0;
	end = vdd*config_get_real ("xcell.waveform.rise_high")/100.0;
      }
      else {
	st = vdd*config_get_real("xcell.waveform.fall_high")/100.0;
	end = vdd*config_get_real("xcell.waveform.fall_low")/100.0;
      }
      fprintf (sfp, "* in[%d] %s; out[%d] %s\n",
	       dyn[j].in_id, dyn[j].in_init ? "fall" : "rise",
	       dyn[j].out_id, dyn[j].out_init ? "fall" : "rise");
      fprintf (sfp, ".measure tran transit_%d_%d trig V(p%d) VAL=%g TD=%gp CROSS=1 targ V(p%d) VAL=%g\n", j, ns, _get_output_pin (dyn[j].out_id), st, off,
	       _get_output_pin (dyn[j].out_id), end);
      
      /*
	3. Measure internal power
      */
      fprintf (sfp, ".measure tran intpow_%d_%d avg i(Vv1) from %gp to %gp\n",
	       j, ns, off, off + window);
      tm++;
    }
  }

  fprintf (sfp, "\n.end\n");
  fclose (sfp);

  snprintf (buf, 1024, "%s %s.spi > %s.log 2>&1",
	    config_get_string ("xcell.spice_binary"), file, file);
  system (buf);

  /* -- open measurements, and save data -- */
  for (int nload=0; nload < nsweep; nload++) {
    struct Hashtable *H;
    hash_bucket_t *b;
    hash_iter_t hi;
    
    if (is_xyce()) {
      snprintf (buf, 1024, "%s.spi.mt%d", file, nload);
    }
    else {
      snprintf (buf, 1024, "%s.mt0", file);
    }

    if (is_xyce()) {
      H = parse_measurements (buf);
    }
    else {
      H = parse_measurements (buf, "load", nload);
    }
    if (!H) {
      fatal_error ("Could not open measurement output file %s.\n", buf);
    }

    hash_iter_init (H, &hi);
    while ((b = hash_iter_next (H, &hi))) {
      char *s = b->key;
      int type, off;
      int i, j;
      double v;
      
      if (strncasecmp (s, "delay_", 6) == 0) {
	type = 0;
	off = 6;
      }
      else if (strncasecmp (s, "transit_", 8) == 0) {
	type = 1;
	off = 8;
      }
      else if (strncasecmp (s, "intpow_", 7) == 0) {
	type = 2;
	off = 7;
      }
      else if (strncasecmp (s, "negdelay_", 9) == 0) {
	type = 3;
	off = 9;
      }
      else {
	continue;
      }
      if (sscanf (s + off, "%d_%d", &i, &j) != 2) {
	fatal_error ("Could not parse line: %s", buf);
      }
      v = b->f;
      
      Assert (0 <= i && i < A_LEN (dyn), "What?");
      Assert (0 <= j && j < nslew, "What?");

      if (v == -1) {
	/* -- measurement error -- */
	continue;
      }

      if (type == 0 || type == 3) {
	if (fabs(v - window*config_get_real("xcell.units.time_conv")) >
	    fabs(v)) {
	  if (type == 0) {
	    dyn[i].delay[j+nload*nslew] = v;
	  }
	  else {
	    dyn[i].delay[j+nload*nslew] = -v;
	  }
	  //printf ("[%d] delay %d %d = %g\n", type, i, j, v/1e-12);
	}
      }
      else if (type == 1) {
	dyn[i].transit[j+nload*nslew] = v;
	//printf ("   transit %d %d = %g\n", i, j, v/1e-12);
      }
      else if (type == 2) {
	dyn[i].intpow[j+nload*nslew] = -v*vdd;
      }
    }
    hash_free (H);
  }

  unlink_generic (file);

  /* Xyce creates multiple measurement files */
  if (is_xyce()) {
    /* -- other measurement files -- */
    for (int i=1; i < config_get_table_size ("xcell.load"); i++) {
      snprintf (buf, 1024, "%s.spi.mt%d", file, i);
      unlink (buf);
    }
    snprintf (buf, 1024, "%s.spi.res", file);
    unlink (buf);
  }

  return 1;
}



void Cell::_emit_dynamic ()
{
  int nslew = config_get_table_size ("xcell.input_trans");
  int nsweep = config_get_table_size ("xcell.load");
  double window = config_get_real ("xcell.short_window");
  
  char buf[1024];
  
  if (!nl) return;
  
  for (int nout=0; nout < _num_outputs; nout++) {
    char buf[1024];
    CNLFP (_lfp, "pin(");
    _sprint_output_pin (buf, 1024, nout);
    a->mfprintf (_lfp, "%s", buf);
    fprintf (_lfp, ") {\n");
    _l->_tab ();
    
    CNLFP (_lfp, "direction : output;\n");

    if (!_is_dataflow) {
      CNLFP (_lfp, "function : ");
      fprintf (_lfp, "\"");
      if (!is_stateholding) {
	int first = 1;
	/*-- print function --*/
	for (int i=0; i < (1 << _num_inputs); i++) {
	  if (bitset_tst (_outvals[nout], i)) {
	    if (!first) {
	      fprintf (_lfp, "+");
	    }
	    first = 0;
	    _print_input_case (i);
	  }
	}
      }
      else {
	int first = 1;

	if (is_out[nout] == 0) {
	  /*-- this output is the state-holding gate --*/
	  for (int i=0; i < (1 << _num_inputs); i++) {
	    if (bitset_tst (st[1], i)) {
	      if (!first) {
		fprintf (_lfp, "+");
	      }
	      first = 0;
	      _print_input_case (i);
	    }
	  }
	  if (!first) {
	    fprintf (_lfp, "+");
	    first = 0;
	  }
	  a->mfprintf (_lfp, "%s", buf);
	  fprintf (_lfp, "*!(");
	  first = 1;
	  for (int i=0; i < (1 << _num_inputs); i++) {
	    if (bitset_tst (st[0], i)) {
	      if (!first) {
		fprintf (_lfp, "+");
	      }
	      first = 0;
	      _print_input_case (i);
	    }
	  }
	  fprintf (_lfp, ")");
	}
	else if (is_out[nout] == 1) {
	  /*-- this output is the state-holding gate --*/
	  for (int i=0; i < (1 << _num_inputs); i++) {
	    if (bitset_tst (st[0], i)) {
	      if (!first) {
		fprintf (_lfp, "+");
	      }
	      first = 0;
	      _print_input_case (i);
	    }
	  }
	  if (!first) {
	    fprintf (_lfp, "+");
	  }
	  first = 0;
	  a->mfprintf (_lfp, "%s", buf);
	  fprintf (_lfp, "*!(");
	  first = 1;
	  for (int i=0; i < (1 << _num_inputs); i++) {
	    if (bitset_tst (st[1], i)) {
	      if (!first) {
		fprintf (_lfp, "+");
	      }
	      first = 0;
	      _print_input_case (i);
	    }
	  }
	  fprintf (_lfp, ")");
	}
	else {
	  fprintf (_lfp, " ");
	}
      }
      fprintf (_lfp, "\";\n");
    }

    /* -- measurement results -- 


       internal_power() {
         related_pin : "...";
	 when : "others";
	 rise_power (power_8x8) {
	  index_1(...)
	  index_2(...)
	  values("...")
         }
       }
       timing() {
         related_pin : "...";
	 timing_sense : positive_unate, negative_unate, non_unate ;
	 when : "...";
	 cell_rise(delay8x8) {
	 }
	 rise_transition(delay_8x8) {
	 }
       }
     */
    for (int i=0; i < A_LEN (dyn); i++) {
      int idx_case;
      
      if (dyn[i].out_id != nout) continue;

      /* -- internal power -- */

      CNLFP (_lfp, "internal_power() {\n");
      _l->_tab();

      CNLFP (_lfp, "related_pin: \"");
      _sprint_input_pin (buf, 1024, dyn[i].in_id);
      a->mfprintf (_lfp, "%s", buf);
      fprintf (_lfp, "\";\n");

      idx_case = dyn[i].idx[dyn[i].nidx-1];
      if (_num_inputs > 1) {
	CNLFP (_lfp, "when : \"");
	_print_input_case (idx_case, (1 << dyn[i].in_id));
	fprintf (_lfp, "\";\n");
      }

      CNLFP (_lfp, "%s_power(power_%dx%d) {\n",
	     dyn[i].in_init == 0 ? "rise" : "fall",
	     config_get_table_size ("xcell.input_trans"),
	     config_get_table_size ("xcell.load"));
      _l->_tab();
      _l->dump_index_tables ();
      
      CNLFP (_lfp, "values(\\\n"); _l->_tab();
      for (int j=0; j < nslew; j++) {
	CNLFP (_lfp, "\"");
	for (int k=0; k < nsweep; k++) {
	  double dp;
	  if (k != 0) {
	    fprintf (_lfp, ", ");
	  }
	  /*-- XXX: fixme: units, internal power definition --*/
	  dp = dyn[i].intpow[j + k*nslew];
	  dp = dp - leakage_power[idx_case];
	  /* internal power is always in fJ */
	  dp = dp*window*1e-12;	/* picoseconds * power */
	  dp /= 1e-15;
	  fprintf (_lfp, "%g", dp);
	}
	fprintf (_lfp, "\"");
	if (j != nslew-1) {
	  fprintf (_lfp, ",");
	}
	fprintf (_lfp, "\\\n");
      }
      _l->_untab();
      CNLFP (_lfp, ");\n");

      _l->_untab();
      CNLFP (_lfp, "}\n");
      
      _l->_untab();
      CNLFP (_lfp, "}\n");


      CNLFP (_lfp, "timing() {\n");
      _l->_tab();

      CNLFP (_lfp, "related_pin: \"");
      _sprint_input_pin (buf, 1024, dyn[i].in_id);
      a->mfprintf (_lfp, "%s", buf);
      fprintf (_lfp, "\";\n");

      if (dyn[i].in_init == dyn[i].out_init) {
	CNLFP (_lfp, "timing_sense : positive_unate;\n");
      }
      else {
	CNLFP (_lfp, "timing_sense : negative_unate;\n");
      }

      idx_case = dyn[i].idx[dyn[i].nidx-1];
      if (_num_inputs > 1) {
	CNLFP (_lfp, "when : \"");
	_print_input_case (idx_case, (1 << dyn[i].in_id));
	fprintf (_lfp, "\";\n");
      }

      /* -- cell rise -- */
      
      CNLFP (_lfp, "cell_%s(delay_%dx%d) {\n",
	     dyn[i].in_init == 0 ? "rise" : "fall",
	     config_get_table_size ("xcell.input_trans"),
	     config_get_table_size ("xcell.load"));
      _l->_tab();
      _l->dump_index_tables ();

      CNLFP (_lfp, "values(\\\n"); _l->_tab();
      for (int j=0; j < nslew; j++) {
	CNLFP (_lfp, "\"");
	for (int k=0; k < nsweep; k++) {
	  double dp;
	  if (k != 0) {
	    fprintf (_lfp, ", ");
	  }
	  /*-- XXX: fixme: units, internal power definition --*/
	  dp = dyn[i].delay[j+k*nslew];
	  dp /= config_get_real ("xcell.units.time_conv");
	  if (dp < 0) {
	    dp = 0;
	  }
	  fprintf (_lfp, "%g", dp);
	}
	fprintf (_lfp, "\"");
	if (j != nslew-1) {
	  fprintf (_lfp, ",");
	}
	fprintf (_lfp, "\\\n");
      }
      _l->_untab();
      CNLFP (_lfp, ");\n");

      _l->_untab();
      CNLFP (_lfp, "}\n");

      /* -- transition time -- */

      CNLFP (_lfp, "%s_transition(delay_%dx%d) {\n",
	     dyn[i].in_init == 0 ? "rise" : "fall",
	     config_get_table_size ("xcell.input_trans"),
	     config_get_table_size ("xcell.load"));
      _l->_tab();
      _l->dump_index_tables ();

      CNLFP (_lfp, "values(\\\n"); _l->_tab();
      for (int j=0; j < nslew; j++) {
	CNLFP (_lfp, "\"");
	for (int k=0; k < nsweep; k++) {
	  double dp;
	  if (k != 0) {
	    fprintf (_lfp, ", ");
	  }
	  /*-- XXX: fixme: units, internal power definition --*/
	  dp = dyn[i].transit[j+k*nslew];
	  dp /= config_get_real ("xcell.units.time_conv");
	  fprintf (_lfp, "%g", dp);
	}
	fprintf (_lfp, "\"");
	if (j != nslew-1) {
	  fprintf (_lfp, ",");
	}
	fprintf (_lfp, "\\\n");
      }
      _l->_untab();
      CNLFP (_lfp, ");\n");

      _l->_untab();
      CNLFP (_lfp, "}\n");

      
      _l->_untab();
      CNLFP (_lfp, "}\n");
    }

    _l->_untab ();
    CNLFP (_lfp, "}\n");
  }
}



void Cell::_sprint_input_pin (char *buf, int sz, int pos)
{
  int i = _get_input_pin (pos);
  ActId *tmp = nl->bN->ports[pos].c->toid();
  tmp->sPrint (buf, sz);
  delete tmp;

  if (_is_dataflow) {
    snprintf (buf + strlen (buf), sz - strlen (buf), ":i");
  }
}

void Cell::_sprint_output_pin (char *buf, int sz, int pos)
{
  int i = _get_output_pin (pos);
  ActId *tmp = nl->bN->ports[pos].c->toid();
  tmp->sPrint (buf, sz);
  delete tmp;

  if (_is_dataflow) {
    snprintf (buf + strlen (buf), sz - strlen (buf), ":o");
  }
}

int Cell::_run_dflow_leakage (void)
{
  MALLOC (leakage_power, double, (1 << _num_inputs));
  for (int i=0; i < (1 << _num_inputs); i++) {
    leakage_power[i] = 0;
  }
  return 1;
}

int Cell::_run_dflow_dynamic (void)
{
  int nslew = config_get_table_size ("xcell.input_trans");
  int nsweep = config_get_table_size ("xcell.load");
  
  for (int i=0; i < _num_inputs; i++) {
    for (int j=0; j < _num_outputs; j++) {

      A_NEW (dyn, struct dynamic_case);
      A_NEXT (dyn).nidx = 0;
      A_NEXT (dyn).out_id = j;
      A_NEXT (dyn).in_id = i;
      A_NEXT (dyn).in_init = 1;
      A_NEXT (dyn).out_init = 1;

      MALLOC (A_NEXT (dyn).delay, double, nslew*nsweep);
      MALLOC (A_NEXT (dyn).transit, double, nslew*nsweep);
      MALLOC (A_NEXT (dyn).intpow, double, nslew*nsweep);

      for (int k=0; k < nslew*nsweep; k++) {
	A_NEXT (dyn).delay[k] = 100e-12;
	A_NEXT (dyn).transit[k] = 10e-12;
	A_NEXT (dyn).intpow[k] = 0.0;
      }
      A_INC (dyn);
    }
  }
  
  return 1;
}

int Cell::_run_dflow_input_cap (void)
{
  if (_num_inputs > 0) {
    MALLOC (time_up, double, _num_inputs);
    MALLOC (time_dn, double, _num_inputs);
    for (int i=0; i < _num_inputs; i++) {
      time_up[i] = 0;
      time_dn[i] = 0;
    }
  }
  return 1;
}
  
