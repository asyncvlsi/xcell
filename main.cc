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
#include "liberty.h"

int main (int argc, char **argv)
{
  Act *a;
  char buf[1024];
  list_t *l;

  l = list_new ();
  list_append (l, "xcell.conf");
  list_append (l, "lint.conf");

  Act::Init (&argc, &argv, l);

  list_free (l);

  if (argc != 3) {
    fatal_error ("Usage: %s <act-cell-file> <libname>\n", argv[0]);
  }
  a = new Act (argv[1]);
  a->Expand ();

  config_set_default_int ("net.emit_parasitics", 1);

  ActNetlistPass *np = new ActNetlistPass (a);
  np->run();

  Liberty L(argv[2]);
  
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

      Cell *c = new Cell (&L, p);
      c->characterize();
      c->emit();
      delete c;
    }
  }
  return 0;
}  
