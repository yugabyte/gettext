/* XML resource locating rules
   Copyright (C) 2015 Free Software Foundation, Inc.

   This file was written by Daiki Ueno <ueno@gnu.org>, 2015.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

/* Specification.  */
#include "locating-rule.h"

#include "basename.h"
#include "concat-filename.h"

#if HAVE_DIRENT_H
# include <dirent.h>
#endif

#if HAVE_DIRENT_H
# define HAVE_DIR 1
#else
# define HAVE_DIR 0
#endif

#include <errno.h>
#include "error.h"
#include <fnmatch.h>
#include "gettext.h"
#include "hash.h"
#include <libxml/parser.h>
#include <libxml/uri.h>
#include "xalloc.h"

#define _(str) gettext (str)

#define LOCATING_RULES_NS "https://www.gnu.org/s/gettext/ns/locating-rules/1.0"

struct document_locating_rule_ty
{
  char *ns;
  char *local_name;

  char *target;
};

struct document_locating_rule_list_ty
{
  struct document_locating_rule_ty *items;
  size_t nitems;
  size_t nitems_max;
};

struct locating_rule_ty
{
  char *pattern;

  struct document_locating_rule_list_ty doc_rules;
  char *target;
};

struct locating_rule_list_ty
{
  char *base;

  struct locating_rule_ty *items;
  size_t nitems;
  size_t nitems_max;
};

static char *
get_attribute (xmlNode *node, const char *attr)
{
  xmlChar *value;
  char *result;

  value = xmlGetProp (node, BAD_CAST attr);
  result = xstrdup ((const char *) value);
  xmlFree (value);

  return result;
}

static const char *
document_locating_rule_match (struct document_locating_rule_ty *rule,
                              xmlDoc *doc)
{
  xmlNode *root;

  root = xmlDocGetRootElement (doc);
  if (rule->ns != NULL)
    {
      if (root->ns == NULL
          || !xmlStrEqual (root->ns->href, BAD_CAST rule->ns))
        return NULL;
    }

  if (rule->local_name != NULL)
    {
      if (!xmlStrEqual (root->name,
                        BAD_CAST rule->local_name))
        return NULL;
    }

  return rule->target;
}

static const char *
locating_rule_match (struct locating_rule_ty *rule,
                     const char *filename)
{
  if (fnmatch (rule->pattern, filename, FNM_PATHNAME) != 0)
    return NULL;

  if (rule->target != NULL)
    return rule->target;

  if (rule->doc_rules.nitems > 0)
    {
      xmlDoc *doc;
      size_t i;

      doc = xmlReadFile (filename, "utf-8",
                         XML_PARSE_NONET
                         | XML_PARSE_NOWARNING
                         | XML_PARSE_NOBLANKS
                         | XML_PARSE_NOERROR);
      if (doc == NULL)
        return NULL;

      for (i = 0; i < rule->doc_rules.nitems; i++)
        {
          const char *target =
            document_locating_rule_match (&rule->doc_rules.items[i], doc);
          if (target)
            return target;
        }
    }

  return NULL;
}

char *
locating_rule_list_locate (struct locating_rule_list_ty *rules,
                           const char *path)
{
  const char *target = NULL;
  size_t i;

  for (i = 0; i < rules->nitems; i++)
    {
      target = locating_rule_match (&rules->items[i], path);
      if (target != NULL)
        return xconcatenated_filename (rules->base, target, NULL);
    }

  return NULL;
}

static void
missing_attribute (const char *element, const char *attribute)
{
  error (0, 0, _("\"%s\" node does not have \"%s\""), element, attribute);
}

static void
document_locating_rule_destroy (struct document_locating_rule_ty *rule)
{
  free (rule->ns);
  free (rule->local_name);
  free (rule->target);
}

static void
document_locating_rule_list_add (struct document_locating_rule_list_ty *rules,
                                 xmlNode *node)
{
  struct document_locating_rule_ty rule;

  if (!xmlHasProp (node, BAD_CAST "target"))
    {
      missing_attribute ("documentRule", "target");
      return;
    }

  memset (&rule, 0, sizeof (struct document_locating_rule_ty));

  if (xmlHasProp (node, BAD_CAST "ns"))
    rule.ns = get_attribute (node, "ns");
  if (xmlHasProp (node, BAD_CAST "localName"))
    rule.local_name = get_attribute (node, "localName");
  rule.target = get_attribute (node, "target");

  if (rules->nitems == rules->nitems_max)
    {
      rules->nitems_max = 2 * rules->nitems_max + 1;
      rules->items =
        xrealloc (rules->items,
                  sizeof (struct document_locating_rule_ty)
                  * rules->nitems_max);
    }
  memcpy (&rules->items[rules->nitems++], &rule,
          sizeof (struct document_locating_rule_ty));
}

static void
locating_rule_destroy (struct locating_rule_ty *rule)
{
  size_t i;

  for (i = 0; i < rule->doc_rules.nitems; i++)
    document_locating_rule_destroy (&rule->doc_rules.items[i]);

  free (rule->pattern);
  free (rule->target);
}

static bool
locating_rule_list_add_file (struct locating_rule_list_ty *rules,
                             const char *rule_file_name)
{
  xmlDoc *doc;
  xmlNode *root, *node;

  doc = xmlReadFile (rule_file_name, "utf-8",
                     XML_PARSE_NONET
                     | XML_PARSE_NOWARNING
                     | XML_PARSE_NOBLANKS
                     | XML_PARSE_NOERROR);
  if (doc == NULL)
    {
      error (0, 0, _("cannot read XML file %s"), rule_file_name);
      return false;
    }

  root = xmlDocGetRootElement (doc);
  if (!(xmlStrEqual (root->name, BAD_CAST "locatingRules")
#if 0
        && root->ns
        && xmlStrEqual (root->ns->href, BAD_CAST LOCATING_RULES_NS)
#endif
        ))
    {
      error (0, 0, _("the root element is not \"locatingRules\""));
      xmlFreeDoc (doc);
      return false;
    }

  for (node = root->children; node; node = node->next)
    {
      if (xmlStrEqual (node->name, BAD_CAST "locatingRule"))
        {
          struct locating_rule_ty rule;

          if (!xmlHasProp (node, BAD_CAST "pattern"))
            {
              missing_attribute ("locatingRule", "pattern");
              xmlFreeDoc (doc);
              continue;
            }

          memset (&rule, 0, sizeof (struct locating_rule_ty));
          rule.pattern = get_attribute (node, "pattern");
          if (xmlHasProp (node, BAD_CAST "target"))
            rule.target = get_attribute (node, "target");
          else
            {
              xmlNode *n;

              for (n = node->children; n; n = n->next)
                {
                  if (xmlStrEqual (n->name, BAD_CAST "documentRule"))
                    document_locating_rule_list_add (&rule.doc_rules, n);
                }
            }
          if (rules->nitems == rules->nitems_max)
            {
              rules->nitems_max = 2 * rules->nitems_max + 1;
              rules->items =
                xrealloc (rules->items,
                          sizeof (struct locating_rule_ty) * rules->nitems_max);
            }
          memcpy (&rules->items[rules->nitems++], &rule,
                  sizeof (struct locating_rule_ty));
        }
    }

  xmlFreeDoc (doc);
  return true;
}

static bool
locating_rule_list_add_directory (struct locating_rule_list_ty *rules,
                                  const char *directory)
{
#if HAVE_DIR
  DIR *dirp;

  dirp = opendir (directory);
  if (dirp == NULL)
    return false;

  for (;;)
    {
      struct dirent *dp;

      errno = 0;
      dp = readdir (dirp);
      if (dp != NULL)
        {
          const char *name = dp->d_name;
          size_t namlen = strlen (name);

          if (namlen > 4 && memcmp (name + namlen - 4, ".loc", 4) == 0)
            {
              char *locator_file_name =
                xconcatenated_filename (directory, name, NULL);
              locating_rule_list_add_file (rules, locator_file_name);
              free (locator_file_name);
            }
        }
      else if (errno != 0)
        return false;
      else
        break;
    }
  if (closedir (dirp))
    return false;

#endif
  return true;
}

struct locating_rule_list_ty *
locating_rule_list_alloc (const char *base, const char *directory)
{
  struct locating_rule_list_ty *result;

  xmlCheckVersion (LIBXML_VERSION);

  result = XCALLOC (1, struct locating_rule_list_ty);
  result->base = xstrdup (base);

  locating_rule_list_add_directory (result, directory);

  return result;
}

void
locating_rule_list_destroy (struct locating_rule_list_ty *rules)
{
  free (rules->base);

  while (rules->nitems-- > 0)
    locating_rule_destroy (&rules->items[rules->nitems]);
  free (rules->items);
}

void
locating_rule_list_free (struct locating_rule_list_ty *rules)
{
  if (rules != NULL)
    locating_rule_list_destroy (rules);
  free (rules);
}