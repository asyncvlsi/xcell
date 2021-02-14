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
#include <unistd.h>
#include <config.h>
#include <atrace.h>
#include <act/act.h>
#include <act/passes.h>

#define NLFP  line(fp); fprintf

static int tabs = 0;

void line(FILE *fp)
{
  for (int i=0; i < tabs; i++) {
    fprintf (fp, "   ");
  }
}

void tab (void)
{
  tabs++;
}

void untab (void)
{
  Assert (tabs > 0, "Hmm");
  tabs--;
}


int gen_spice_header (FILE *fp, Act *a, ActNetlistPass *np, Process *p)
{
  netlist_t *nl;
  A_DECL (int, xout);

  A_INIT (xout);

  fprintf (fp, "**************************\n");
  fprintf (fp, "** characterization run **\n");
  fprintf (fp, "**************************\n");

  /*-- XXX: power and ground are actually in the netlist --*/

  fprintf (fp, ".include '%s'\n\n", config_get_string ("xcell.tech_setup"));
  fprintf (fp, ".global Vdd\n");
  fprintf (fp, ".global GND\n");

  fprintf (fp, ".param load = 2.2f\n");
  fprintf (fp, ".param resistor = %g%s\n\n",
	   config_get_real ("xcell.R_value"),
	   config_get_string ("xcell.units.resis"));

  /*-- dump subcircuit --*/
  np->Print (fp, p);

  /*-- instantiate circuit --*/

  fprintf (fp, "\n\n");

  nl = np->getNL (p);
  
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
  a->mfprintfproc (fp, p);
  fprintf (fp, "\n");

  if (A_LEN (xout) == 0) {
    warning ("Cell %s: no outputs?\n", p->getName());
    return 0;
  }

  /*-- power supplies --*/
  fprintf (fp, "Vv0 GND 0 0.0\n");
  fprintf (fp, "Vv1 Vdd 0 %g\n", config_get_real ("xcell.Vdd"));

  /*-- load cap on output that is swept --*/
  for (int i=0; i < A_LEN (xout); i++) {
    fprintf (fp, "Clc%d p%d 0 load\n\n", i, xout[i]);
  }
  A_FREE (xout);

  return 1;
}


int get_num_inputs (netlist_t *nl)
{
  int num_inputs = 0;
  for (int i=0; i < A_LEN (nl->bN->ports); i++) {
    if (nl->bN->ports[i].omit) continue;
    if (nl->bN->ports[i].input) {
      num_inputs++;
    }
  }
  if (num_inputs == 0) {
    warning ("Cell %s: no inputs?", nl->bN->p->getName());
    return 0;
  }

  if (num_inputs > 8) {
    warning ("High fan-in cell?");
    return 0;
  }
  return num_inputs;
}

L_A_DECL (act_booleanized_var_t *, _sh_vars);
static bitset_t *_tt[2];
static bitset_t **_outvals; 	// value of outputs + _sh_vars
static int _num_outputs;

void _add_support_var (netlist_t *nl, ActId *id)
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

void _collect_support (netlist_t *nl, act_prs_expr_t *e)
{
  if (!e) return;
  switch (e->type) {
  case ACT_PRS_EXPR_AND:
  case ACT_PRS_EXPR_OR:
    _collect_support (nl, e->u.e.l);
    _collect_support (nl, e->u.e.r);
    break;

  case ACT_PRS_EXPR_NOT:
    _collect_support (nl, e->u.e.l);
    break;

  case ACT_PRS_EXPR_LABEL:
    warning ("labels within state-holding gate...");
    break;

  case ACT_PRS_EXPR_TRUE:
  case ACT_PRS_EXPR_FALSE:
    break;

  case ACT_PRS_EXPR_VAR:
    _add_support_var (nl, e->u.v.id);
    break;
    
  default:
    fatal_error ("Unexpected type %d", e->type);
    break;
  }
}

int _eval_expr (netlist_t *nl, act_prs_expr_t *e, unsigned int v)
{
  act_connection *c;
  if (!e) return 0;
  switch (e->type) {
  case ACT_PRS_EXPR_AND:
    if (_eval_expr (nl, e->u.e.l, v) && _eval_expr (nl, e->u.e.r, v)) {
      return 1;
    }
    return 0;
    break;
  case ACT_PRS_EXPR_OR:
    if (_eval_expr (nl, e->u.e.l, v) || _eval_expr (nl, e->u.e.r, v)) {
      return 1;
    }
    return 0;
    break;

  case ACT_PRS_EXPR_NOT:
    return 1 - _eval_expr (nl, e->u.e.l, v);
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

void _build_truth_table (netlist_t *nl, act_prs_expr_t *e, int idx)
{
  for (unsigned int i=0; i < (1 << A_LEN (_sh_vars)); i++) {
    if (_eval_expr (nl, e, i)) {
      bitset_set (_tt[idx], i);
    }
    else {
      bitset_clr (_tt[idx], i);
    }
  }
}

/*
 *
 *  Create a testbench to generate all the leakage scenarios
 *
 */
int run_leakage_scenarios (FILE *fp,
			   Act *a, ActNetlistPass *np, Process *p)
{
  FILE *sfp;
  netlist_t *nl;
  int num_inputs;
  char buf[1024];
  A_DECL (char *, outname);

  A_INIT (outname);

  printf ("Analyzing leakage for %s\n", p->getName());
  
  nl = np->getNL (p);

  num_inputs = get_num_inputs (nl);
  if (num_inputs == 0) {
    return 0;
  }

  sfp = fopen ("_spicelk_.spi", "w");
  if (!sfp) {
    fatal_error ("Could not open _spicelk_.spi for writing");
  }

  /* -- std header that instantiates the module -- */
  
  if (!gen_spice_header (sfp, a, np, p)) {
    fclose (sfp);
    return 0;
  }

  double vdd = config_get_real ("xcell.Vdd");
  double period = config_get_real ("xcell.period");

  /* -- leakage scenarios -- */
  
  for (int k=0; k < num_inputs; k++) {
    int pos;
    pos = k;
    for (int j=0; j < A_LEN (nl->bN->ports); j++) {
      if (nl->bN->ports[j].omit) continue;
      if (!nl->bN->ports[j].input) continue;
      if (pos == 0) {
	pos = j;
	break;
      }
      pos--;
    }

    fprintf (sfp, "Vn%d p%d 0 PWL (0p 0 1000p 0\n", k, pos);
    int tm = 1;
    for (int i=0; i < (1 << num_inputs); i++) {
      if ((i >> k) & 0x1) {
	fprintf (sfp, "+%gp %g %gp %g\n", tm*period + 1, vdd, (tm+1)*period, vdd);
      }
      else {
	fprintf (sfp, "+%gp 0 %gp 0\n", tm*period + 1, (tm+1)*period);
      }
      tm ++;
    }
    fprintf (sfp, "+)\n\n");
  }

  fprintf (sfp, "\n");

  /* -- measurement of current -- */
  int tm = 1;
  double lk_window = config_get_real ("xcell.leak_window");
  if (period / (2 * lk_window) < 2) {
    fatal_error ("leak_window parameter (%g) is too large for period (%g)\n",
		 lk_window, period);
  }
  for (int i=0; i < (1 << num_inputs); i++) {
    fprintf (sfp, ".measure tran current_%d avg i(Vv1) from %gp to %gp\n",
	     i, tm*period + lk_window, (tm+1)*period - lk_window);
    fprintf (sfp, ".measure tran leak_%d PARAM='-current_%d*%g'\n", i, i,
	     vdd);
    tm++;
  }

  fprintf (sfp, ".tran 10p %gp\n", tm*period);
  if (config_exists ("xcell.extra_sp_txt")) {
    char **x = config_get_table_string ("xcell.extra_sp_txt");
    for (int i=0; i < config_get_table_size ("xcell.extra_sp_txt"); i++) {
      fprintf (sfp, "%s\n", x[i]);
    }
  }
  fprintf (sfp, ".print tran %s", config_get_string ("xcell.extra_print_stmt"));
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
  _num_outputs = num_outputs;

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
  
  snprintf (buf, 1024, "%s _spicelk_.spi > _spicelk_.log 2>&1",
	    config_get_string ("xcell.spice_binary"));
  system (buf);

  /* -- extract results from spice run -- */

  /* -- convert trace file to atrace format -- */
  if (config_get_int ("xcell.spice_output_fmt") == 0) {
    /* raw */
    snprintf (buf, 1024, "tr2alint -r _spicelk_.spi.raw _spicelk_");
  }
  else {
    snprintf (buf, 1024, "tr2alint _spicelk_.spi.tr0 _spicelk_");
  }
  system (buf);

  /* 
     Step 1: truth tables
  */
  snprintf (buf, 1024, "_spicelk_");
  atrace *tr = atrace_open (buf);
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
      printf ("Output node `%s' not found", outname[i]);
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
    _outvals[i] = bitset_new (1 << num_inputs);
  }
  
  for (int i=0; i < (1 << num_inputs); i++) {
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
	  warning ("out[%d]: X (%g) @ %g\n", j, val, ATRACE_NODE_VAL (tr, timenode));
	}
      }
    }
    atrace_advance_time (tr, 1000e-12/ATRACE_GET_STEPSIZE (tr));
  }

  atrace_close (tr);

  /*
    Step 2: leakage measurements
  */
  sfp = fopen ("_spicelk_.spi.mt0", "r");
  if (!sfp) {
    fatal_error ("Could not open measurement output from Xyce\n");
  }
  while (fgets (buf, 1024, sfp)) {
    double lk;
    char *s = strtok (buf, " \t");
    if (strncasecmp (s, "leak", 4) == 0) {
      int i;
      if (sscanf (s + 5, "%d", &i) != 1) {
	fatal_error ("Could not parse line: %s", buf);
      }
      s = strtok (NULL, " \t");
      if (strcmp (s, "=") != 0) {
	fatal_error ("Could not parse line: %s", buf);
      }
      s = strtok (NULL, " \t");
      if (sscanf (s, "%lg", &lk) != 1) {
	fatal_error ("Could not parse line: %s", buf);
      }

      /*-- XXX: now check for interference --*/

      /*
	Primary inputs are set by "i"
	Secondary inputs are set by bitset outval[.]
      */

      /* XXX: HERE */
      
      NLFP (fp, "leakage_power() {\n");
      tab();
      NLFP (fp, "when : \"");
      for (int k=0; k < num_inputs; k++) {
	int pos;
	pos = k;
	for (int j=0; j < A_LEN (nl->bN->ports); j++) {
	  if (nl->bN->ports[j].omit) continue;
	  if (!nl->bN->ports[j].input) continue;
	  if (pos == 0) {
	    pos = j;
	    break;
	  }
	  pos--;
	}

	ActId *tmp;
	tmp = nl->bN->ports[pos].c->toid();
	tmp->sPrint (buf, 1024);
	delete tmp;

	if (k > 0) {
	  fprintf (fp, "&");
	}

	if (((i >> k) & 1) == 0) {
	  fprintf (fp, "!");
	}
	a->mfprintf (fp, "%s", buf);
      }
      fprintf (fp, "\";\n");
      NLFP (fp, "value : %g;\n", lk/config_get_real ("xcell.units.power_conv"));
      untab();
      NLFP (fp, "}\n");
    }
  }
  fclose (sfp);

#if 1
  unlink ("_spicelk_.spi");
  unlink ("_spicelk_.spi.mt0");
  unlink ("_spicelk_.spi.raw");
  unlink ("_spicelk_.spi.tr0");
  unlink ("_spicelk_.log");
  unlink ("_spicelk_.trace");
  unlink ("_spicelk_.names");
#endif

  A_FREE (outnode);
  
  return 1;
}


int run_input_cap_scenarios (FILE *fp,
			     Act *a, ActNetlistPass *np, Process *p)

{
  FILE *sfp;
  netlist_t *nl;
  int num_inputs;
  char buf[1024];

  printf ("Analyzing input capacitance for %s\n", p->getName());

  nl = np->getNL (p);

  num_inputs = get_num_inputs (nl);
  if (num_inputs == 0) {
    return 0;
  }

  sfp = fopen ("_spicecap_.spi", "w");
  if (!sfp) {
    fatal_error ("Could not open _spicecap_.spi for writing");
  }
  if (!gen_spice_header (sfp, a, np, p)) {
    fclose (sfp);
    return 0;
  }

  


  fclose (sfp);

  

  return 1;
}

int run_spice (FILE *lfp, Act *a, ActNetlistPass *np, Process *p)
{
  FILE *fp;
  netlist_t *nl;
  node_t *is_stateholding = NULL;

  nl = np->getNL (p);
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
	    return 0;
	  }
	}
      }
    }
  }

  if (is_stateholding) {
    /* -- analyze state-holding operator -- */
    Assert (is_stateholding->v, "What?");
    A_INIT (_sh_vars);
    _collect_support (nl, is_stateholding->v->e_up);
    _collect_support (nl, is_stateholding->v->e_dn);
    Assert (A_LEN (_sh_vars) > 0, "What?!");

    _tt[0] = bitset_new (1 << A_LEN (_sh_vars));
    _tt[1] = bitset_new (1 << A_LEN (_sh_vars));
    bitset_clear (_tt[0]);
    bitset_clear (_tt[1]);
    
    _build_truth_table (nl, is_stateholding->v->e_up, 1);
    _build_truth_table (nl, is_stateholding->v->e_dn, 0);
  }

  /* -- add leakage information to the .lib file -- */
  if (!run_leakage_scenarios (lfp, a, np, p)) {
    return 0;
  }

  /* -- XXX: the leakage scenarios should also record
          - internal signals
	  - output value
  */

  printf ("%s stateholding: %d\n", p->getName(), is_stateholding ? 1 : 0);

  /* -- dynamic scenarios -- */
  
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
  
  if (A_LEN (_sh_vars) > 0) {
    bitset_free (_tt[0]);
    bitset_free (_tt[1]);
  }
  A_FREE (_sh_vars);

  return 1;
  
  fp = fopen ("_spicedy_.spi", "w");
  if (!fp) {
    fatal_error ("Could not open _spicedy_.spi for writing");
  }
  /* -- std header that instantiates the module -- */
  if (!gen_spice_header (fp, a, np, p)) {
    fclose (fp);
    return 0;
  }


  /* Run dynamic scenarios 

   - combinational logic:
        a : 0 -> 1 
	  For each assignment to others where the 0->1 transition
        changes the output, emit case

	same thing for 1 -> 0

   - state-holding logic, single gate
   
       analyze the production rule for the gate

       figure out the values of the other signals in all the leakage
       cases
       
  */
     
     

  
  
  fclose (fp);
  return 1;
}


void lib_emit_template (FILE *fp, const char *name, const char *prefix,
			const char *v_trans, const char *v_load)
{
  NLFP (fp, "%s_template(%s_%dx%d) {\n", name,  prefix, 
	config_get_table_size (v_trans),
	config_get_table_size (v_load));
  tab();
  NLFP (fp, "variable_1 : input_net_transition;\n");
  NLFP (fp, "variable_2 : total_output_net_capacitance;\n");
  NLFP (fp, "index_1(\"");
  double *dtable = config_get_table_real (v_trans);
  for (int i=0; i < config_get_table_size (v_trans); i++) {
    if (i != 0) {
      fprintf (fp, ", ");
    }
    fprintf (fp, "%g", dtable[i]);
  }
  fprintf (fp, "\");\n");

  NLFP (fp, "index_2(\"");
  dtable = config_get_table_real (v_load);
  for (int i=0; i < config_get_table_size (v_load); i++) {
    if (i != 0) {
      fprintf (fp, ", ");
    }
    fprintf (fp, "%g", dtable[i]);
  }
  fprintf (fp, "\");\n");
  untab();
  NLFP (fp, "}\n");
}

/*------------------------------------------------------------------------
 *
 *  lib_emit_header --
 *
 *   Emit Synopsys .lib file header
 *
 *------------------------------------------------------------------------
 */
void lib_emit_header (FILE *fp, char *name)
{
  fprintf (fp, "/* --- Synopsys format .lib file --- */\n");
  fprintf (fp, "/*\n");
  fprintf (fp, "      Process: %g\n", config_get_real ("xcell.P_value"));
  fprintf (fp, "      Voltage: %g V\n", config_get_real ("xcell.Vdd"));
  fprintf (fp, "  Temperature: %g K\n", config_get_real ("xcell.T"));
  fprintf (fp, "\n*/\n\n");

  fprintf (fp, "library(%s) {\n", name);
  tab();
  NLFP (fp, "technology(cmos);\n");

  NLFP (fp, "delay_model : table_lookup;\n");
  NLFP (fp, "nom_process : %g;\n", config_get_real ("xcell.P_value"));
  NLFP (fp, "nom_voltage : %g;\n", config_get_real ("xcell.Vdd"));
  NLFP (fp, "nom_temperature : %g;\n", config_get_real ("xcell.T"));

  NLFP (fp, "time_unit : \"1%ss\";\n", config_get_string ("xcell.units.time"));
  NLFP (fp, "voltage_unit : \"1V\";\n");
  NLFP (fp, "current_unit : \"1%sA\";\n", config_get_string ("xcell.units.current"));
  NLFP (fp, "pulling_resistance_unit : \"1kohm\";\n");
  NLFP (fp, "capacitive_load_unit (1, \"%sF\");\n", config_get_string ("xcell.units.cap"));
  NLFP (fp, "leakage_power_unit : \"1%sW\";\n", config_get_string ("xcell.units.power"));
  NLFP (fp, "internal_power_unit : \"1fJ\";\n");

  NLFP (fp, "default_connection_class : \"default\";\n");
  NLFP (fp, "default_fanout_load : 1;\n");
  NLFP (fp, "default_inout_pin_cap : 0.01;\n");
  NLFP (fp, "default_input_pin_cap : 0.01;\n");
  NLFP (fp, "default_output_pin_cap : 0;\n");
  NLFP (fp, "default_fanout_load : 1;\n");
  
  NLFP (fp, "input_threshold_pct_fall : 50;\n");
  NLFP (fp, "input_threshold_pct_rise : 50;\n");
  NLFP (fp, "output_threshold_pct_fall : 50;\n");
  NLFP (fp, "output_threshold_pct_rise : 50;\n");

  NLFP (fp, "slew_derate_from_library : 1;\n");

  NLFP (fp, "slew_lower_threshold_pct_fall : %g;\n",
	config_get_real ("xcell.waveform.fall_low"));
  NLFP (fp, "slew_lower_threshold_pct_rise : %g;\n",
	config_get_real ("xcell.waveform.rise_low"));

  NLFP (fp, "slew_upper_threshold_pct_fall : %g;\n",
	config_get_real ("xcell.waveform.fall_high"));
  NLFP (fp, "slew_upper_threshold_pct_rise : %g;\n",
	config_get_real ("xcell.waveform.rise_high"));
  
  NLFP (fp, "default_max_transition : %g;\n",
	config_get_real ("xcell.default_max_transition_time"));

  /* -- operating conditions -- */
  char **table = config_get_table_string ("xcell.corners");
  NLFP (fp, "operating_conditions(\"%s\") {\n", table[0]);
  tab();
  NLFP (fp, "process : %g;\n", config_get_real ("xcell.P_value"));
  NLFP (fp, "temperature : %g;\n", config_get_real ("xcell.T"));
  NLFP (fp, "voltage : %g;\n", config_get_real ("xcell.Vdd"));
  NLFP (fp, "process_corner: \"%s\";\n", table[0]);
  NLFP (fp, "tree_type : balanced_tree;\n");
  untab();
  NLFP (fp, "}\n");
  NLFP (fp, "default_operating_conditions : typical;\n");

  /* -- wire load -- */
  NLFP (fp,"wire_load(\"wlm1\") {\n");
  tab();
  for (int i=0; i < 6; i++) {
    NLFP (fp, "fanout_capacitance(%d, %d);\n", i, i);
  }
  untab();
  NLFP (fp, "}\n");
  NLFP (fp, "default_wire_load : \"wlm1\";\n");

  /* -- templates -- */
  lib_emit_template (fp, "lu_table", "delay",
		     "xcell.input_trans", "xcell.load");

  lib_emit_template (fp, "power_lut", "power",
		     "xcell.input_trans_power", "xcell.load_power");

}


void lib_emit_footer (FILE *fp, char *name)
{
  untab();
  line (fp); fprintf (fp, "}\n");
}

int main (int argc, char **argv)
{
  Act *a;
  FILE *fp;
  char buf[1024];

  Act::Init (&argc, &argv);

  if (argc != 3) {
    fatal_error ("Usage: %s <act-cell-file> <libname>\n", argv[0]);
  }
  a = new Act (argv[1]);
  a->Expand ();

  ActCellPass *cp = new ActCellPass (a);

  ActNetlistPass *np = new ActNetlistPass (a);
  np->run();

  config_read ("xcell.conf");
  config_read ("lint.conf");

  snprintf (buf, 1024, "%s.lib", argv[2]);

  fp = fopen (buf, "w");
  if (!fp) {
    fatal_error ("Could not open file `%s' for writing", argv[1]);
  }

  /* -- emit header -- */
  lib_emit_header (fp, argv[2]);

  UserDef  *topu = a->Global()->findType ("characterize<>");
  if (!topu) {
    fatal_error ("File `%s': missing top-level characterize process and instance", argv[1]);
  }
  
  Process *top = dynamic_cast<Process *> (topu);
  if (!top) {
    fatal_error ("File `%s': missing top-level characterize process", argv[1]);
  }

  for (int i=1; i; i++) {
    char buf[1024];
    netlist_t *nl;
    InstType *it;
    Process *p;

    snprintf (buf, 1024, "g%d", i);
    it = top->Lookup (buf);
    if (!it) {
      /* done */
      break;
    }

    if (TypeFactory::isProcessType (it)) {
      p = dynamic_cast<Process *>(it->BaseType());

      nl = np->getNL (p);
      if (!nl) {
	fatal_error ("Cells file needs to instantiate all cells. %s missing.",
		     p->getName());
      }

      /*-- characterize cell --*/

      NLFP (fp, "cell(");
      a->mfprintfproc (fp, p);
      fprintf (fp, ") {\n");
      tab();

      /* XXX what is this? */
      NLFP (fp, "area : 25;\n");

      /* XXX: fixme emit power and ground pins */
      NLFP (fp, "pg_pin(GND) {\n");
      tab();
      NLFP (fp, "pg_type : primary_ground;\n");
      untab();
      NLFP (fp, "}\n");
    
      NLFP (fp, "pg_pin(Vdd) {\n");
      tab();
      NLFP (fp, "pg_type : primary_power;\n");
      untab();
      NLFP (fp, "}\n");

      /* -- we now need to run spice -- */
      run_spice (fp, a, np, p);

      untab();
      NLFP (fp, "}\n");
    }
  }

  lib_emit_footer (fp, argv[2]);
  fclose (fp);
  
  return 0;
}  
