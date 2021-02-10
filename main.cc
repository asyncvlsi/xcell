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
#include <config.h>
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


netlist_t *gen_spice_header (FILE *fp, Act *a, ActNetlistPass *np, Process *p)
{
  netlist_t *nl;

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
  int idx_out = -1;
  for (int i=0; i < A_LEN (nl->bN->ports); i++) {
    if (nl->bN->ports[i].omit) continue;
    if (!nl->bN->ports[i].input) {
      if (idx_out == -1) {
	idx_out = i;
      }
      else {
	warning ("Extend to multi-output gates!");
	return NULL;
      }
    }
    fprintf (fp, "p%d ", i);
  }
  a->mfprintfproc (fp, p);
  fprintf (fp, "\n");

  if (idx_out == -1) {
    warning ("Cell %s: no outputs?\n", p->getName());
    return NULL;
  }

  /*-- power supplies --*/
  fprintf (fp, "Vv0 GND 0 0.0\n");
  fprintf (fp, "Vv1 Vdd 0 %g\n", config_get_real ("xcell.Vdd"));

  /*-- load cap on output that is swept --*/
  fprintf (fp, "Clc p%d 0 load\n\n", idx_out);

  return nl;
}


/*
 *
 *  Create a testbench to generate all the leakage scenarios
 *
 */
double *run_leakage_scenarios (FILE *fp,
			       Act *a, ActNetlistPass *np, Process *p)
{
  FILE *sfp;
  netlist_t *nl;
  int num_inputs;
  char buf[1024];
  double *ret;

  printf ("Analyzing leakage for %s\n", p->getName());

  nl = np->getNL (p);

  num_inputs = 0;
  for (int i=0; i < A_LEN (nl->bN->ports); i++) {
    if (nl->bN->ports[i].omit) continue;
    if (nl->bN->ports[i].input) {
      num_inputs++;
    }
  }
  if (num_inputs == 0) {
    warning ("Cell %s: no inputs?", p->getName());
    return NULL;
  }

  if (num_inputs > 8) {
    warning ("High fan-in cell?");
    return NULL;
  }

  sfp = fopen ("_spicelk_.spi", "w");
  if (!sfp) {
    fatal_error ("Could not open _spicelk_.spi for writing");
  }

  /* -- std header that instantiates the module -- */
  nl = gen_spice_header (sfp, a, np, p);
  if (!nl) {
    fclose (sfp);
    return NULL;
  }

  double vdd = config_get_real ("xcell.Vdd");

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
	fprintf (sfp, "+%dp %g %dp %g\n", tm*10000 + 1, vdd, (tm+1)*10000, vdd);
      }
      else {
	fprintf (sfp, "+%dp 0 %dp 0\n", tm*10000 + 1, (tm+1)*10000);
      }
      tm ++;
    }
    fprintf (sfp, "+)\n\n");
  }

  fprintf (sfp, "\n");

  /* -- measurement of current -- */

  int tm = 1;
  for (int i=0; i < (1 << num_inputs); i++) {
    fprintf (sfp, ".measure tran current_%d avg i(Vv1) from %dp to %dp\n",
	     i, tm*10000 + 1000, (tm+1)*10000 - 1000);
    fprintf (sfp, ".measure tran leak_%d PARAM='-current_%d*%g'\n", i, i,
	     vdd);
    tm++;
  }

  fprintf (sfp, ".tran 10p %dp\n", tm*10000);
  /*fprintf (fp, ".option post\n");*/
  fprintf (sfp, ".end\n");


  fclose (sfp);
  
  /* -- run the spice simulation -- */
  system ("Xyce _spicelk_.spi > _spicelk_.log 2>&1");

  /* -- extract results from spice run --
     NOTE: we can use this to build the truth table for any group of
     combinational gates!
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
  
  return ret;
}


int run_spice (FILE *lfp, Act *a, ActNetlistPass *np, Process *p)
{
  FILE *fp;
  netlist_t *nl;
  int is_stateholding = 0;

  nl = np->getNL (p);
  for (node_t *n = nl->hd; n; n = n->next) {
    if (n->v) {
      if (n->v->e_up || n->v->e_dn) {
	/* there's a gate here */
	if (n->v->stateholding &&
	    (!n->v->unstaticized || n->v->manualkeeper != 0)) {
	  is_stateholding++;
	  break;
	}
      }
    }
  }

  if (!run_leakage_scenarios (lfp, a, np, p)) {
    return 0;
  }

  /* dump leakage info */
  

  printf ("%s stateholding: %d\n", p->getName(), is_stateholding);

  /* -- dynamic scenarios -- */
  if (is_stateholding > 1) {
    warning ("Cannot handle modules with more than one state-holding gate at present.");
    return 0;
  }

  return 1;
  
  fp = fopen ("_spicedy_.spi", "w");
  if (!fp) {
    fatal_error ("Could not open _spicedy_.spi for writing");
  }
  /* -- std header that instantiates the module -- */
  nl = gen_spice_header (fp, a, np, p);
  if (!nl) {
    fclose (fp);
    return 0;
  }
  
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

      /* -- leakage power -- */

      /* -- input pins -- */

      /* -- output pins -- */

      untab();
      NLFP (fp, "}\n");
    }
  }

  lib_emit_footer (fp, argv[2]);
  fclose (fp);
  
  return 0;
}  
