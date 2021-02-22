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
#include <math.h>
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

  fprintf (fp, ".temp %g\n", config_get_real ("xcell.T") - 273.15);

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

/**
   _sh_vars is the support for the state-holding variable
**/
L_A_DECL (act_booleanized_var_t *, _sh_vars);

/**
   _tt[1], _tt[0] : pull-up/pull-down truth table, numbering
   corresponding to _sh_vars[]
**/
static bitset_t *_tt[2];

static bitset_t **_outvals; 	// value of outputs, followed by +
				// _sh_vars

static int _num_outputs;	/* number of outputs */

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

int get_input_pin (netlist_t *nl, int pin)
{
  int pos = pin;
  int j;
  for (j=0; j < A_LEN (nl->bN->ports); j++) {
    if (nl->bN->ports[j].omit) continue;
    if (!nl->bN->ports[j].input) continue;
    if (pos == 0) {
      return j;
    }
    pos--;
  }
  Assert (0, "Could not find input pin!");
  return -1;
}

int get_output_pin (netlist_t *nl, int pin)
{
  int pos = pin;
  int j;
  for (j=0; j < A_LEN (nl->bN->ports); j++) {
    if (nl->bN->ports[j].omit) continue;
    if (nl->bN->ports[j].input) continue;
    if (pos == 0) {
      return j;
    }
    pos--;
  }
  Assert (0, "Could not find output pin!");
  return -1;
}


void print_all_input_cases (FILE *sfp,
			    const char *prefix,
			    int num_inputs, netlist_t *nl)
{
  double vdd = config_get_real ("xcell.Vdd");
  double period = config_get_real ("xcell.period");

  /* -- leakage scenarios -- */
  
  for (int k=0; k < num_inputs; k++) {
    fprintf (sfp, "Vn%d %s%d 0 PWL (0p 0 1000p 0\n", k, prefix, k);
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
}


void print_input_cap_cases (FILE *sfp,
			    const char *prefix,
			    int num_inputs, netlist_t *nl)
{
  double vdd = config_get_real ("xcell.Vdd");
  double period = config_get_real ("xcell.period");
  double window = config_get_real ("xcell.cap_window");

  for (int i=0; i < num_inputs; i++) {

    fprintf (sfp, "Vn%d %s%d 0 PWL (0p 0 1000p 0\n", i, prefix, i);
    
    /*-- we run input i up and down 2 times, with the others being in
      all possible different states --*/
    int tm = 1;

    /* prefix: just go through all possible cases */
    for (int p=0; p < i; p++) {
      for (int q=0; q < (1 << (num_inputs-1)); q++) {
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
    for (int q=0; q < (1 << (num_inputs-1)); q++) {
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
	warning ("Period (%g) needs to be >= cap_window (%g)*5\n", period, window);
      }
    }

    /* -- now rest -- */
    for (int p=i+1; p < num_inputs; p++) {
      for (int q=0; q < (1 << (num_inputs-1)); q++) {
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


/*
 *
 *  Create a testbench to generate all the leakage scenarios
 *
 */
int run_leakage_scenarios (FILE *fp,
			   Act *a, ActNetlistPass *np, Process *p,
			   node_t *is_stateholding)
{
  FILE *sfp;
  netlist_t *nl;
  int num_inputs;
  char buf[1024];
  A_DECL (char *, outname);

  A_INIT (outname);

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

  /* -- generate all possible static input scenarios -- */
  print_all_input_cases (sfp, "p", num_inputs, nl);
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
    snprintf (buf, 1024, "tr2alint _spicelk_.tr0 _spicelk_");
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
	  warning ("%s: out[%d]: X (%g) @ %g\n", p->getName(), j, val, ATRACE_NODE_VAL (tr, timenode));
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
    sfp = fopen ("_spicelk_.mt0", "r");
    if (!sfp) {
      fatal_error ("Could not open measurement output file.\n");
    }
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

      if (lk < 0) {
	warning ("%s: measurement failed for leakage, scenario %d (%g)",
		 p->getName(), i, lk);
      }

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
      
      NLFP (fp, "leakage_power() {\n");
      tab();
      NLFP (fp, "when : \"");
      for (int k=0; k < num_inputs; k++) {
	int pos = get_input_pin (nl, k);

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
  unlink ("_spicelk_.log");
  unlink ("_spicelk_.trace");
  unlink ("_spicelk_.names");

  /* -- Xyce -- */
  unlink ("_spicelk_.spi.mt0");
  unlink ("_spicelk_.spi.raw");

  /* -- hspice -- */
  unlink ("_spicelk_.mt0");
  unlink ("_spicelk_.tr0");
  unlink ("_spicelk_.st0");
  unlink ("_spicelk_.ic0");
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

  /* -- resis to input -- */

  int pos = 0;
  for (int i=0; i < A_LEN (nl->bN->ports); i++) {
    if (nl->bN->ports[i].omit) continue;
    if (!nl->bN->ports[i].input) continue;
    fprintf (sfp, "Rdrv%d q%d p%d resistor\n", pos, pos, pos);
    pos++;
  }
  fprintf (sfp, "\n");

  print_input_cap_cases (sfp, "q", num_inputs, nl);

  /* measure input delays! */

  double period = config_get_real ("xcell.period");
  double vdd = config_get_real ("xcell.Vdd");
  double window = config_get_real ("xcell.cap_window");
    
  for (int i=0; i < num_inputs; i++) {
    for (int j=0; j < ((1 << (num_inputs-1))); j++) {
      double my_start = i*(1 << (num_inputs-1))*period + period + period*j;
      
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

  fprintf (sfp, ".tran 10p %gp\n",  num_inputs * ((1 << (num_inputs-1))) * period + period);
  
  fprintf (sfp, "\n.end\n");
  
  fclose (sfp);

  snprintf (buf, 1024, "%s _spicecap_.spi > _spicecap_.log 2>&1",
	    config_get_string ("xcell.spice_binary"));
  system (buf);

  double *time_up;
  double *time_dn;
  int *upcnt, *dncnt;

  MALLOC (upcnt, int, num_inputs);
  MALLOC (dncnt, int, num_inputs);
  MALLOC (time_up, double, num_inputs);
  MALLOC (time_dn, double, num_inputs);
  for (int i=0; i < num_inputs; i++) {
    time_up[i] = 0;
    time_dn[i] = 0;
    upcnt[i] = 0;
    dncnt[i] = 0;
  }

  sfp = fopen ("_spicecap_.spi.mt0", "r");
  if (!sfp) {
    sfp = fopen ("_spicecap_.mt0", "r");
    if (!sfp) {
      fatal_error ("Could not open measurement output from simulation.\n");
    }
  }
  while (fgets (buf, 1024, sfp)) {
    int i, j;
    double tm;
    char *s = strtok (buf, " \t");
    if (strncasecmp (s, "cap_tup_", 8) == 0) {
      if (sscanf (s + 8, "%d", &i) != 1) {
	fatal_error ("Could not parse line: %s", buf);
      }
      j = 8;
      while (*(s+j) && isdigit (*(s+j))) {
	j++;
      }
      if (*(s+j) != '_') {
	fatal_error ("Could not parse line: %s", buf);
      }
      j++;
      if (sscanf (s+j, "%d", &j) != 1) {
	fatal_error ("Could not parse line: %s", buf);
      }
      s = strtok (NULL, " \t");
      if (strcmp (s, "=") != 0) {
	fatal_error ("Could not parse line: %s", buf);
      }
      s = strtok (NULL, " \t");
      if (sscanf (s, "%lg", &tm) != 1) {
	fatal_error ("Could not parse line: %s", buf);
      }

      if (tm < 0) {
	warning ("%s: measurement failed: time=%g for input cap measurement %d_%d",
		 p->getName(), i, j);
      }

      time_up[i] += tm;
      upcnt[i]++;
    }
    else if (strncasecmp (s, "cap_tdn_", 8) == 0) {
      if (sscanf (s + 8, "%d", &i) != 1) {
	fatal_error ("Could not parse line: %s", buf);
      }
      j = 8;
      while (*(s+j) && isdigit (*(s+j))) {
	j++;
      }
      if (*(s+j) != '_') {
	fatal_error ("Could not parse line: %s", buf);
      }
      j++;
      if (sscanf (s+j, "%d", &j) != 1) {
	fatal_error ("Could not parse line: %s", buf);
      }
      s = strtok (NULL, " \t");
      if (strcmp (s, "=") != 0) {
	fatal_error ("Could not parse line: %s", buf);
      }
      s = strtok (NULL, " \t");
      if (sscanf (s, "%lg", &tm) != 1) {
	fatal_error ("Could not parse line: %s", buf);
      }

      time_dn[i] += tm;
      dncnt[i]++;
    }
  }
  fclose (sfp);

  for (int i=0; i < num_inputs; i++) {
    if (upcnt[i] != dncnt[i]) {
      warning ("What?!");
    }
    if (i > 0 && upcnt[i] != upcnt[i-1]) {
      warning ("What?!!");
    }
    if (upcnt[i] == 0 || dncnt[i] == 0) {
      warning ("Bad news");
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

    int pos = get_input_pin (nl, i);

    ActId *tmp;
    tmp = nl->bN->ports[pos].c->toid();
    tmp->sPrint (buf, 1024);
    delete tmp;
    NLFP (fp, "pin(");  a->mfprintf (fp, "%s", buf);  fprintf (fp, ") {\n");
    tab();
    NLFP (fp, "direction : input;\n");
    NLFP (fp, "rise_capacitance : %g;\n", time_up[i]*1e15);
    NLFP (fp, "fall_capacitance : %g;\n", time_dn[i]*1e15);
    untab();
    NLFP (fp, "}\n");
  }

#if 1
  unlink ("_spicecap_.spi");
  unlink ("_spicecap_.log");

  /* Xyce */
  unlink ("_spicecap_.spi.mt0");

  /* hspice */
  unlink ("_spicecap_.mt0");
  unlink ("_spicecap_.st0");
  unlink ("_spicecap_.ic0");
#endif
  

  return 1;
}


/*------------------------------------------------------------------------
 *
 * Dynamic scenarios
 *
 *------------------------------------------------------------------------
 */
int run_dynamic (FILE *fp, Act *a, ActNetlistPass *np, netlist_t *nl,
		 node_t *sh)
{
  FILE *sfp;
  bitset_t *st[2];
  int num_inputs;
  int sh_outvar;
  
  num_inputs = get_num_inputs (nl);
  if (num_inputs == 0) {
    return 0;
  }

  /* Run dynamic scenarios.

     If there's a state-holding node, build state-holding truth table
     from the existing truth tables.

     The assumption is that the state-holding gate is fully controlled
     from the primary input.
  */
  if (sh) {
    int *iomap;
    int *xmap;
    int xcount;
    
    /* -- we know the truth table for the pull-up and pull-down, so
          create it based directly on input 
     --*/
    st[0] = bitset_new (num_inputs); /* pull-down */
    bitset_clear (st[0]);
    st[1] = bitset_new (num_inputs); /* pull-up */
    bitset_clear (st[1]);

    MALLOC (iomap, int, num_inputs + _num_outputs);
    for (int i=0; i < num_inputs + _num_outputs; i++) {
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
      int opos = num_inputs;
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
      if (pos >= num_inputs && opos >= (num_inputs + _num_outputs)) {
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

    for (int i=0; i < (1 << num_inputs); i++) {
      unsigned int idx = 0;

      for (int j=0; j < num_inputs; j++) {
	if (iomap[j] != -1) {
	  if ((i >> j) & 0x1) {
	    idx |= (1 << iomap[j]);
	  }
	}
      }

      for (int j=0; j < _num_outputs; j++) {
	if (iomap[num_inputs + j] != -1) {
	  if (bitset_tst (_outvals[j], i)) {
	    idx |= (1 << iomap[num_inputs + j]);
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

    int *is_out;
    MALLOC (is_out, int, _num_outputs);
    for (int i=0; i < _num_outputs; i++) {
      is_out[i] = -1;
    }
    /* -1 = new, -2 = no, 0 = non-inv, 1 = inv */

    for (int i=0; i < (1 << num_inputs); i++) {
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
  
  sfp = fopen ("_spicedy_.spi", "w");
  if (!sfp) {
    fatal_error ("Could not open _spicedy_.spi for writing");
  }

  /* -- std header that instantiates the module -- */
  if (!gen_spice_header (sfp, a, np, nl->bN->p)) {
    fclose (sfp);
    return 0;
  }

  /* -- create spice scenarios -- */
  for (int nout=0; nout < _num_outputs; nout++) {
    int pos = get_output_pin (nl, nout);

    for (int i=0; i < (1 << num_inputs); i++) {
      /* -- current input vector scenario: i -- */
    
      for (int j=0; j < num_inputs; j++) {
	/* -- current bit being checked: j -- */
	unsigned int opp = i ^ (1 << j);

	if (sh) {
	  /* st[0], st[1] has pull-up and pull-down */

	}
	else {
	  /*-- combinational logic --*/
	  
	  /* _outvals has truth table for output */
	  if ((!!bitset_tst (_outvals[nout], i)) !=
	      (!!bitset_tst (_outvals[nout], opp))) {

	    /* Found transition! Characterize this transition */
	    

	  }
	}
      }
    }
  }

  for (int nout=0; nout < _num_outputs; nout++) {
    int pos = get_output_pin (nl, nout);
    ActId *tmp;
    char buf[1024];
    NLFP (fp, "pin(");
    tmp = nl->bN->ports[pos].c->toid();
    tmp->sPrint (buf, 1024);
    delete tmp;
    a->mfprintf (fp, "%s", buf);

    fprintf (fp, ") {\n");
    tab ();
    NLFP (fp, "direction : output;\n");
    NLFP (fp, "function : ");
    if (!sh) {
      int first = 1;
      /*-- print function --*/
      fprintf (fp, "\"");
      for (int i=0; i < (1 << num_inputs); i++) {
	if (bitset_tst (_outvals[nout], i)) {
	  if (!first) {
	    fprintf (fp, "+");
	  }
	  first = 0;
	  for (int j=0; j < num_inputs; j++) {
	    int ipin = get_input_pin (nl, j);

	    if (j != 0) {
	      fprintf (fp, "*");
	    }
	    
	    if (!((i >> j) & 1)) {
	      fprintf (fp, "!");
	    }
	    tmp = nl->bN->ports[ipin].c->toid();
	    tmp->sPrint (buf, 1024);
	    delete tmp;
	    a->mfprintf (fp, "%s", buf);
	  }
	}
      }
      fprintf (fp, "\";\n");
    }
    else {
      /* XXX: state-holding gate function definition */
      fprintf (fp, " \" \";\n");
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
    
    untab ();
    NLFP (fp, "}\n");
  }
  
  fclose (sfp);


  if (sh) {
    bitset_free (st[0]);
    bitset_free (st[1]);
  }

#if 1
  unlink ("_spicedy_.spi");
#endif  
  
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

  printf ("Cell: %s %s\n", p->getName(), is_stateholding ? "(state-holding)" : "");
  
  /* -- add leakage information to the .lib file -- */
  if (!run_leakage_scenarios (lfp, a, np, p, is_stateholding)) {
    return 0;
  }

  /* -- generate input capacitance estimates -- */
  if (!run_input_cap_scenarios (lfp, a, np, p)) {
    return 0;
  }

  /* -- dynamic scenarios for output pins -- */
  if (!run_dynamic (lfp, a, np, nl, is_stateholding)) {
    return 0;
  }

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
  
  if (is_stateholding) {
    bitset_free (_tt[0]);
    bitset_free (_tt[1]);
    _tt[0] = NULL;
    _tt[1] = NULL;
  }
  A_FREE (_sh_vars);

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
