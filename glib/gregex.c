/* GRegex -- regular expression API wrapper around PCRE.
 *
 * Copyright (C) 1999, 2000 Scott Wimer
 * Copyright (C) 2004, Matthias Clasen <mclasen@redhat.com>
 * Copyright (C) 2005 - 2007, Marco Barisione <marco@barisione.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <config.h>

#include "gregex.h"

#include <glib.h>
#include <glib/gi18n.h>
#include <string.h>

#ifdef USE_SYSTEM_PCRE
#include <pcre.h>
#else
#include "pcre/pcre.h"
#endif

#include "galias.h"

/* Mask of all the possible values for GRegexCompileFlags. */
#define G_REGEX_COMPILE_MASK (G_REGEX_CASELESS		| \
			      G_REGEX_MULTILINE		| \
			      G_REGEX_DOTALL		| \
			      G_REGEX_EXTENDED		| \
			      G_REGEX_ANCHORED		| \
			      G_REGEX_DOLLAR_ENDONLY	| \
			      G_REGEX_UNGREEDY		| \
			      G_REGEX_RAW		| \
			      G_REGEX_NO_AUTO_CAPTURE	| \
			      G_REGEX_OPTIMIZE		| \
			      G_REGEX_DUPNAMES		| \
			      G_REGEX_NEWLINE_CR	| \
			      G_REGEX_NEWLINE_LF	| \
			      G_REGEX_NEWLINE_CRLF)

/* Mask of all the possible values for GRegexMatchFlags. */
#define G_REGEX_MATCH_MASK (G_REGEX_MATCH_ANCHORED	| \
			    G_REGEX_MATCH_NOTBOL	| \
			    G_REGEX_MATCH_NOTEOL	| \
			    G_REGEX_MATCH_NOTEMPTY	| \
			    G_REGEX_MATCH_PARTIAL	| \
			    G_REGEX_MATCH_NEWLINE_CR	| \
			    G_REGEX_MATCH_NEWLINE_LF	| \
			    G_REGEX_MATCH_NEWLINE_CRLF	| \
			    G_REGEX_MATCH_NEWLINE_ANY)

/* if the string is in UTF-8 use g_utf8_ functions, else use
 * use just +/- 1. */
#define NEXT_CHAR(re, s) (((re)->compile_opts & PCRE_UTF8) ? \
				g_utf8_next_char (s) : \
				((s) + 1))
#define PREV_CHAR(re, s) (((re)->compile_opts & PCRE_UTF8) ? \
				g_utf8_prev_char (s) : \
				((s) - 1))

struct _GMatchInfo
{
  GRegex *regex;		/* the regex */
  GRegexMatchFlags match_opts;	/* options used at match time on the regex */
  gint matches;			/* number of matching sub patterns */
  gint pos;			/* position in the string where last match left off */
  gint *offsets;		/* array of offsets paired 0,1 ; 2,3 ; 3,4 etc */
  gint n_offsets;		/* number of offsets */
  gint *workspace;		/* workspace for pcre_dfa_exec() */
  gint n_workspace;		/* number of workspace elements */
  const gchar *string;		/* string passed to the match function */
  gssize string_len;		/* length of string */
};

struct _GRegex
{
  volatile guint ref_count;	/* the ref count for the immutable part */
  gchar *pattern;		/* the pattern */
  pcre *pcre_re;		/* compiled form of the pattern */
  GRegexCompileFlags compile_opts;	/* options used at compile time on the pattern */
  GRegexMatchFlags match_opts;	/* options used at match time on the regex */
  pcre_extra *extra;		/* data stored when G_REGEX_OPTIMIZE is used */
};

/* TRUE if ret is an error code, FALSE otherwise. */
#define IS_PCRE_ERROR(ret) ((ret) < PCRE_ERROR_NOMATCH && (ret) != PCRE_ERROR_PARTIAL)

static GRegex	*regex_ref	(GRegex *regex);
static void	 regex_unref	(GRegex *regex);

typedef struct _InterpolationData InterpolationData;
static gboolean	 interpolate_replacement	(const GRegex *regex,
						 const GMatchInfo *match_info,
						 const gchar *string,
						 GString *result,
						 gpointer data);
static GList	*split_replacement		(const gchar *replacement,
						 GError **error);
static void	 free_interpolation_data	(InterpolationData *data);


static const gchar *
match_error (gint errcode)
{
  switch (errcode)
    {
    case PCRE_ERROR_NOMATCH:
      /* not an error */
      break;
    case PCRE_ERROR_NULL:
      /* NULL argument, this should not happen in GRegex */
      g_warning ("A NULL argument was passed to PCRE");
      break;
    case PCRE_ERROR_BADOPTION:
      return "bad options";
    case PCRE_ERROR_BADMAGIC:
      return _("corrupted object");
    case PCRE_ERROR_UNKNOWN_OPCODE:
      return N_("internal error or corrupted object");
    case PCRE_ERROR_NOMEMORY:
      return _("out of memory");
    case PCRE_ERROR_NOSUBSTRING:
      /* not used by pcre_exec() */
      break;
    case PCRE_ERROR_MATCHLIMIT:
      return _("backtracking limit reached");
    case PCRE_ERROR_CALLOUT:
      /* callouts are not implemented */
      break;
    case PCRE_ERROR_BADUTF8:
    case PCRE_ERROR_BADUTF8_OFFSET:
      /* we do not check if strings are valid */
      break;
    case PCRE_ERROR_PARTIAL:
      /* not an error */
      break;
    case PCRE_ERROR_BADPARTIAL:
      return _("the pattern contains items not supported for partial matching");
    case PCRE_ERROR_INTERNAL:
      return _("internal error");
    case PCRE_ERROR_BADCOUNT:
      /* negative ovecsize, this should not happen in GRegex */
      g_warning ("A negative ovecsize was passed to PCRE");
      break;
    case PCRE_ERROR_DFA_UITEM:
      return _("the pattern contains items not supported for partial matching");
    case PCRE_ERROR_DFA_UCOND:
      return _("back references as conditions are not supported for partial matching");
    case PCRE_ERROR_DFA_UMLIMIT:
      /* the match_field field is not used in GRegex */
      break;
    case PCRE_ERROR_DFA_WSSIZE:
      /* handled expanding the workspace */
      break;
    case PCRE_ERROR_DFA_RECURSE:
    case PCRE_ERROR_RECURSIONLIMIT:
      return _("recursion limit reached");
    case PCRE_ERROR_NULLWSLIMIT:
      return _("workspace limit for empty substrings reached");
    case PCRE_ERROR_BADNEWLINE:
      return _("invalid combination of newline flags");
    default:
      break;
    }
  return _("unknown error");
}


/* GMatchInfo */

static GMatchInfo *
match_info_new (const GRegex *regex,
		const gchar  *string,
		gint          string_len,
		gint          start_position,
		gint          match_options,
		gboolean      is_dfa)
{
  GMatchInfo *match_info;

  if (string_len < 0)
    string_len = strlen (string);

  match_info = g_new0 (GMatchInfo, 1);
  match_info->regex = regex_ref ((GRegex *)regex);
  match_info->string = string;
  match_info->string_len = string_len;
  match_info->matches = PCRE_ERROR_NOMATCH;
  match_info->pos = start_position;
  match_info->match_opts = match_options;

  if (is_dfa)
    {
      /* These values should be enough for most cases, if they are not
       * enough g_regex_match_all_full() will expand them. */
      match_info->n_offsets = 24;
      match_info->n_workspace = 100;
      match_info->workspace = g_new (gint, match_info->n_workspace);
    }
  else
    {
      gint capture_count;
      pcre_fullinfo (regex->pcre_re, regex->extra,
                     PCRE_INFO_CAPTURECOUNT, &capture_count);
      match_info->n_offsets = (capture_count + 1) * 3;
    }
  match_info->offsets = g_new0 (gint, match_info->n_offsets);

  return match_info;
}

/**
 * g_match_info_free:
 * @match_info: a #GMatchInfo
 *
 * Frees all the memory associated with the #GMatchInfo structure.
 *
 * Since: 2.14
 */
void
g_match_info_free (GMatchInfo *match_info)
{
  regex_unref (match_info->regex);
  g_free (match_info->offsets);
  g_free (match_info->workspace);
  g_free (match_info);
}

/**
 * g_match_info_next:
 * @match_info: a #GMatchInfo structure
 * @error: location to store the error occuring, or NULL to ignore errors
 *
 * Scans for the next match using the same parameters of the previous
 * call to g_regex_match_full() or g_regex_match() that returned
 * @match_info.
 *
 * The match is done on the string passed to the match function, so you
 * cannot free it before calling this function.
 *
 * Returns: %TRUE is the string matched, %FALSE otherwise
 *
 * Since: 2.14
 */
gboolean
g_match_info_next (GMatchInfo  *match_info,
		   GError     **error)
{
  g_return_val_if_fail (match_info != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (match_info->pos >= 0, FALSE);

  match_info->matches = pcre_exec (match_info->regex->pcre_re,
				   match_info->regex->extra,
				   match_info->string,
				   match_info->string_len,
				   match_info->pos,
				   match_info->regex->match_opts |
				   match_info->match_opts,
				   match_info->offsets,
                                   match_info->n_offsets);
  if (IS_PCRE_ERROR (match_info->matches))
    {
      g_set_error (error, G_REGEX_ERROR, G_REGEX_ERROR_MATCH,
		   _("Error while matching regular expression %s: %s"),
		   match_info->regex->pattern, match_error (match_info->matches));
      return FALSE;
    }

  /* avoid infinite loops if the pattern is an empty string or something
   * equivalent */
  if (match_info->pos == match_info->offsets[1])
    {
      if (match_info->pos > match_info->string_len)
	{
	  /* we have reached the end of the string */
	  match_info->pos = -1;
          match_info->matches = PCRE_ERROR_NOMATCH;
	  return FALSE;
        }
      match_info->pos = NEXT_CHAR (match_info->regex,
				   &match_info->string[match_info->pos]) -
				   match_info->string;
    }
  else
    {
      match_info->pos = match_info->offsets[1];
    }

  return match_info->matches >= 0;
}

/**
 * g_match_info_matches:
 * @match_info: a #GMatchInfo structure
 *
 * Returns: %TRUE if the previous match operation succeeded, %FALSE
 * otherwise
 *
 * Since: 2.14
 */
gboolean
g_match_info_matches (const GMatchInfo *match_info)
{
  g_return_val_if_fail (match_info != NULL, FALSE);

  return match_info->matches >= 0;
}

/**
 * g_match_info_get_match_count:
 * @match_info: a #GMatchInfo structure
 *
 * Retrieves the number of matched substrings (including substring 0, that
 * is the whole matched text), so 1 is returned if the pattern has no
 * substrings in it and 0 is returned if the match failed.
 *
 * If the last match was obtained using the DFA algorithm, that is using
 * g_regex_match_all() or g_regex_match_all_full(), the retrieved
 * count is not that of the number of capturing parentheses but that of
 * the number of matched substrings.
 *
 * Returns:  Number of matched substrings, or -1 if an error occurred
 *
 * Since: 2.14
 */
gint
g_match_info_get_match_count (const GMatchInfo *match_info)
{
  g_return_val_if_fail (match_info, -1);

  if (match_info->matches == PCRE_ERROR_NOMATCH)
    /* no match */
    return 0;
  else if (match_info->matches < PCRE_ERROR_NOMATCH)
    /* error */
    return -1;
  else
    /* match */
    return match_info->matches;
}

/**
 * g_match_info_is_partial_match:
 * @match_info: a #GMatchInfo structure
 *
 * Usually if the string passed to g_regex_match*() matches as far as
 * it goes, but is too short to match the entire pattern, %FALSE is
 * returned. There are circumstances where it might be helpful to
 * distinguish this case from other cases in which there is no match.
 *
 * Consider, for example, an application where a human is required to
 * type in data for a field with specific formatting requirements. An
 * example might be a date in the form ddmmmyy, defined by the pattern
 * "^\d?\d(jan|feb|mar|apr|may|jun|jul|aug|sep|oct|nov|dec)\d\d$".
 * If the application sees the user’s keystrokes one by one, and can
 * check that what has been typed so far is potentially valid, it is
 * able to raise an error as soon as a mistake is made.
 *
 * GRegex supports the concept of partial matching by means of the
 * #G_REGEX_MATCH_PARTIAL flag. When this is set the return code for
 * g_regex_match() or g_regex_match_full() is, as usual, %TRUE
 * for a complete match, %FALSE otherwise. But, when this functions
 * returns %FALSE, you can check if the match was partial calling
 * g_match_info_is_partial_match().
 *
 * When using partial matching you cannot use g_match_info_fetch*().
 *
 * Because of the way certain internal optimizations are implemented the
 * partial matching algorithm cannot be used with all patterns. So repeated
 * single characters such as "a{2,4}" and repeated single metasequences such
 * as "\d+" are not permitted if the maximum number of occurrences is
 * greater than one. Optional items such as "\d?" (where the maximum is one)
 * are permitted. Quantifiers with any values are permitted after
 * parentheses, so the invalid examples above can be coded thus "(a){2,4}"
 * and "(\d)+". If #G_REGEX_MATCH_PARTIAL is set for a pattern that does
 * not conform to the restrictions, matching functions return an error.
 *
 * Returns: %TRUE if the match was partial, %FALSE otherwise
 *
 * Since: 2.14
 */
gboolean
g_match_info_is_partial_match (const GMatchInfo *match_info)
{
  g_return_val_if_fail (match_info != NULL, FALSE);

  return match_info->matches == PCRE_ERROR_PARTIAL;
}

/**
 * g_match_info_expand_references:
 * @match_info: a #GMatchInfo
 * @string_to_expand: the string to expand
 * @error: location to store the error occuring, or %NULL to ignore errors
 *
 * Returns a new string containing the text in @string_to_expand with
 * references expanded. References refer to the last match done with
 * @string against @regex and have the same syntax used by g_regex_replace().
 *
 * The @string_to_expand must be UTF-8 encoded even if #G_REGEX_RAW was
 * passed to g_regex_new().
 *
 * The backreferences are extracted from the string passed to the match
 * function, so you cannot call this function after freeing the string.
 *
 * Returns: the expanded string, or %NULL if an error occurred
 *
 * Since: 2.14
 */
gchar *
g_match_info_expand_references (const GMatchInfo *match_info, 
				const gchar      *string_to_expand,
				GError          **error)
{
  GString *result;
  GList *list;
  GError *tmp_error = NULL;

  g_return_val_if_fail (match_info != NULL, NULL);
  g_return_val_if_fail (string_to_expand != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  list = split_replacement (string_to_expand, &tmp_error);
  if (tmp_error != NULL)
    {
      g_propagate_error (error, tmp_error);
      return NULL;
    }

  result = g_string_sized_new (strlen (string_to_expand));
  interpolate_replacement (match_info->regex, match_info,
			   match_info->string, result, list);

  g_list_foreach (list, (GFunc)free_interpolation_data, NULL);
  g_list_free (list);

  return g_string_free (result, FALSE);
}

/**
 * g_match_info_fetch:
 * @match_info: #GMatchInfo structure
 * @match_num: number of the sub expression
 *
 * Retrieves the text matching the @match_num<!-- -->'th capturing parentheses.
 * 0 is the full text of the match, 1 is the first paren set, 2 the second,
 * and so on.
 *
 * If @match_num is a valid sub pattern but it didn't match anything (e.g.
 * sub pattern 1, matching "b" against "(a)?b") then an empty string is
 * returned.
 *
 * If the match was obtained using the DFA algorithm, that is using
 * g_regex_match_all() or g_regex_match_all_full(), the retrieved
 * string is not that of a set of parentheses but that of a matched
 * substring. Substrings are matched in reverse order of length, so 0 is
 * the longest match.
 *
 * The string is fetched from the string passed to the match function,
 * so you cannot call this function after freeing the string.
 *
 * Returns: The matched substring, or %NULL if an error occurred.
 *          You have to free the string yourself
 *
 * Since: 2.14
 */
gchar *
g_match_info_fetch (const GMatchInfo *match_info,
		    gint              match_num)
{
  /* we cannot use pcre_get_substring() because it allocates the
   * string using pcre_malloc(). */
  gchar *match = NULL;
  gint start, end;

  g_return_val_if_fail (match_info != NULL, NULL);
  g_return_val_if_fail (match_num >= 0, NULL);

  /* match_num does not exist or it didn't matched, i.e. matching "b"
   * against "(a)?b" then group 0 is empty. */
  if (!g_match_info_fetch_pos (match_info, match_num, &start, &end))
    match = NULL;
  else if (start == -1)
    match = g_strdup ("");
  else
    match = g_strndup (&match_info->string[start], end - start);

  return match;
}

/**
 * g_match_info_fetch_pos:
 * @match_info: #GMatchInfo structure
 * @match_num: number of the sub expression
 * @start_pos: pointer to location where to store the start position
 * @end_pos: pointer to location where to store the end position
 *
 * Retrieves the position of the @match_num<!-- -->'th capturing parentheses.
 * 0 is the full text of the match, 1 is the first paren set, 2 the second,
 * and so on.
 *
 * If @match_num is a valid sub pattern but it didn't match anything (e.g.
 * sub pattern 1, matching "b" against "(a)?b") then @start_pos and @end_pos
 * are set to -1 and %TRUE is returned.
 *
 * If the match was obtained using the DFA algorithm, that is using
 * g_regex_match_all() or g_regex_match_all_full(), the retrieved
 * position is not that of a set of parentheses but that of a matched
 * substring. Substrings are matched in reverse order of length, so 0 is
 * the longest match.
 *
 * Returns: %TRUE if the position was fetched, %FALSE otherwise. If the
 *          position cannot be fetched, @start_pos and @end_pos are left
 *          unchanged.
 *
 * Since: 2.14
 */
gboolean
g_match_info_fetch_pos (const GMatchInfo *match_info,
			gint              match_num,
			gint             *start_pos,
			gint             *end_pos)
{
  g_return_val_if_fail (match_info != NULL, FALSE);
  g_return_val_if_fail (match_num >= 0, FALSE);
 
  /* make sure the sub expression number they're requesting is less than
   * the total number of sub expressions that were matched. */
  if (match_num >= match_info->matches)
    return FALSE;

  if (start_pos != NULL)
    *start_pos = match_info->offsets[2 * match_num];

  if (end_pos != NULL)
    *end_pos = match_info->offsets[2 * match_num + 1];

  return TRUE;
}

/**
 * g_match_info_fetch_named:
 * @match_info: #GMatchInfo structure
 * @name: name of the subexpression
 *
 * Retrieves the text matching the capturing parentheses named @name.
 *
 * If @name is a valid sub pattern name but it didn't match anything (e.g.
 * sub pattern "X", matching "b" against "(?P&lt;X&gt;a)?b") then an empty
 * string is returned.
 *
 * The string is fetched from the string passed to the match function,
 * so you cannot call this function after freeing the string.
 *
 * Returns: The matched substring, or %NULL if an error occurred.
 *          You have to free the string yourself
 *
 * Since: 2.14
 */
gchar *
g_match_info_fetch_named (const GMatchInfo *match_info,
			  const gchar      *name)
{
  /* we cannot use pcre_get_named_substring() because it allocates the
   * string using pcre_malloc(). */
  gint num;

  g_return_val_if_fail (match_info != NULL, NULL);
  g_return_val_if_fail (name != NULL, NULL);

  num = g_regex_get_string_number (match_info->regex, name);
  if (num == -1)
    return NULL;
  else
    return g_match_info_fetch (match_info, num);
}

/**
 * g_match_info_fetch_named_pos:
 * @match_info: #GMatchInfo structure
 * @name: name of the subexpression
 * @start_pos: pointer to location where to store the start position
 * @end_pos: pointer to location where to store the end position
 *
 * Retrieves the position of the capturing parentheses named @name.
 *
 * If @name is a valid sub pattern name but it didn't match anything (e.g.
 * sub pattern "X", matching "b" against "(?P&lt;X&gt;a)?b") then @start_pos and
 * @end_pos are set to -1 and %TRUE is returned.
 *
 * Returns: %TRUE if the position was fetched, %FALSE otherwise. If the
 *          position cannot be fetched, @start_pos and @end_pos are left
 *          unchanged.
 *
 * Since: 2.14
 */
gboolean
g_match_info_fetch_named_pos (const GMatchInfo *match_info,
			      const gchar      *name,
			      gint             *start_pos,
			      gint             *end_pos)
{
  gint num;

  g_return_val_if_fail (match_info != NULL, FALSE);
  g_return_val_if_fail (name != NULL, FALSE);
 
  num = g_regex_get_string_number (match_info->regex, name);
  if (num == -1)
    return FALSE;

  return g_match_info_fetch_pos (match_info, num, start_pos, end_pos);
}

/**
 * g_match_info_fetch_all:
 * @match_info: a #GMatchInfo structure
 *
 * Bundles up pointers to each of the matching substrings from a match
 * and stores them in an array of gchar pointers. The first element in
 * the returned array is the match number 0, i.e. the entire matched
 * text.
 *
 * If a sub pattern didn't match anything (e.g. sub pattern 1, matching
 * "b" against "(a)?b") then an empty string is inserted.
 *
 * If the last match was obtained using the DFA algorithm, that is using
 * g_regex_match_all() or g_regex_match_all_full(), the retrieved
 * strings are not that matched by sets of parentheses but that of the
 * matched substring. Substrings are matched in reverse order of length,
 * so the first one is the longest match.
 *
 * The strings are fetched from the string passed to the match function,
 * so you cannot call this function after freeing the string.
 *
 * Returns: a %NULL-terminated array of gchar * pointers. It must be freed
 *          using g_strfreev(). If the previous match failed %NULL is
 *          returned.
 *
 * Since: 2.14
 */
gchar **
g_match_info_fetch_all (const GMatchInfo *match_info)
{
  /* we cannot use pcre_get_substring_list() because the returned value
   * isn't suitable for g_strfreev(). */
  gchar **result;
  gint i;

  g_return_val_if_fail (match_info != NULL, FALSE);

  if (match_info->matches < 0)
    return NULL;

  result = g_new (gchar *, match_info->matches + 1);
  for (i = 0; i < match_info->matches; i++)
    result[i] = g_match_info_fetch (match_info, i);
  result[i] = NULL;

  return result;
}


/* GRegex */

GQuark
g_regex_error_quark (void)
{
  static GQuark error_quark = 0;

  if (error_quark == 0)
    error_quark = g_quark_from_static_string ("g-regex-error-quark");

  return error_quark;
}

static GRegex *
regex_ref (GRegex *regex)
{
  g_atomic_int_inc ((gint*) &regex->ref_count);
  return regex;
}

static void
regex_unref (GRegex *regex)
{
  if (g_atomic_int_exchange_and_add ((gint *) &regex->ref_count, -1) - 1 == 0)
    {
      g_free (regex->pattern);
      if (regex->pcre_re != NULL)
	pcre_free (regex->pcre_re);
      if (regex->extra != NULL)
	pcre_free (regex->extra);
      g_free (regex);
    }
}

/** 
 * g_regex_new:
 * @pattern: the regular expression
 * @compile_options: compile options for the regular expression
 * @match_options: match options for the regular expression
 * @error: return location for a #GError
 * 
 * Compiles the regular expression to an internal form, and does the initial
 * setup of the #GRegex structure.  
 * 
 * Returns: a #GRegex structure
 *
 * Since: 2.14
 */
GRegex *
g_regex_new (const gchar         *pattern, 
 	     GRegexCompileFlags   compile_options,
	     GRegexMatchFlags     match_options,
	     GError             **error)
{
  GRegex *regex;
  pcre *re;
  const gchar *errmsg;
  gint erroffset;
  gboolean optimize = FALSE;
  static gboolean initialized = FALSE;
  
  g_return_val_if_fail (pattern != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);
  g_return_val_if_fail ((compile_options & ~G_REGEX_COMPILE_MASK) == 0, NULL);
  g_return_val_if_fail ((match_options & ~G_REGEX_MATCH_MASK) == 0, NULL);

  if (!initialized)
    {
      gint support;
      const gchar *msg;

      pcre_config (PCRE_CONFIG_UTF8, &support);
      if (!support)
	{
	  msg = N_("PCRE library is compiled without UTF8 support");
	  g_critical (msg);
	  g_set_error (error, G_REGEX_ERROR, G_REGEX_ERROR_COMPILE, gettext (msg));
	  return NULL;
	}

      pcre_config (PCRE_CONFIG_UNICODE_PROPERTIES, &support);
      if (!support)
	{
	  msg = N_("PCRE library is compiled without UTF8 properties support");
	  g_critical (msg);
	  g_set_error (error, G_REGEX_ERROR, G_REGEX_ERROR_COMPILE, gettext (msg));
	  return NULL;
	}

      initialized = TRUE;
    }

  /* G_REGEX_OPTIMIZE has the same numeric value of PCRE_NO_UTF8_CHECK,
   * as we do not need to wrap PCRE_NO_UTF8_CHECK. */
  if (compile_options & G_REGEX_OPTIMIZE)
    optimize = TRUE;

  /* In GRegex the string are, by default, UTF-8 encoded. PCRE
   * instead uses UTF-8 only if required with PCRE_UTF8. */
  if (compile_options & G_REGEX_RAW)
    {
      /* disable utf-8 */
      compile_options &= ~G_REGEX_RAW;
    }
  else
    {
      /* enable utf-8 */
      compile_options |= PCRE_UTF8 | PCRE_NO_UTF8_CHECK;
      match_options |= PCRE_NO_UTF8_CHECK;
    }

  /* PCRE_NEWLINE_ANY is the default for the internal PCRE but
   * not for the system one. */
  if (!(compile_options & G_REGEX_NEWLINE_CR) &&
      !(compile_options & G_REGEX_NEWLINE_LF))
    {
      compile_options |= PCRE_NEWLINE_ANY;
    }

  /* compile the pattern */
  re = pcre_compile (pattern, compile_options, &errmsg, &erroffset, NULL);

  /* if the compilation failed, set the error member and return 
   * immediately */
  if (re == NULL)
    {
      GError *tmp_error = g_error_new (G_REGEX_ERROR, 
				       G_REGEX_ERROR_COMPILE,
				       _("Error while compiling regular "
					 "expression %s at char %d: %s"),
				       pattern, erroffset, errmsg);
      g_propagate_error (error, tmp_error);

      return NULL;
    }

  regex = g_new0 (GRegex, 1);
  regex->ref_count = 1;
  regex->pattern = g_strdup (pattern);
  regex->pcre_re = re;
  regex->compile_opts = compile_options;
  regex->match_opts = match_options;

  if (optimize)
    {
      regex->extra = pcre_study (regex->pcre_re, 0, &errmsg);
      if (errmsg != NULL)
        {
          GError *tmp_error = g_error_new (G_REGEX_ERROR,
                                           G_REGEX_ERROR_OPTIMIZE, 
                                           _("Error while optimizing "
                                             "regular expression %s: %s"),
                                           regex->pattern,
                                           errmsg);
          g_propagate_error (error, tmp_error);
          return NULL;
	}
    }

  return regex;
}

/**
 * g_regex_free:
 * @regex: a #GRegex
 *
 * Frees all the memory associated with the regex structure.
 *
 * Since: 2.14
 */
void
g_regex_free (GRegex *regex)
{
  if (regex == NULL)
    return;

  regex_unref (regex);
}

/**
 * g_regex_get_pattern:
 * @regex: a #GRegex structure
 *
 * Gets the pattern string associated with @regex, i.e. a copy of 
 * the string passed to g_regex_new().
 *
 * Returns: the pattern of @regex
 *
 * Since: 2.14
 */
const gchar *
g_regex_get_pattern (const GRegex *regex)
{
  g_return_val_if_fail (regex != NULL, NULL);

  return regex->pattern;
}

/**
 * g_regex_get_max_backref:
 * @regex: a #GRegex
 *  
 * Returns the number of the highest back reference
 * in the pattern, or 0 if the pattern does not contain
 * back references.
 *
 * Returns: the number of the highest back reference.
 *
 * Since: 2.14
 */
gint
g_regex_get_max_backref (const GRegex *regex)
{
  gint value;

  pcre_fullinfo (regex->pcre_re, regex->extra,
		 PCRE_INFO_BACKREFMAX, &value);

  return value;
}

/**
 * g_regex_get_capture_count:
 * @regex: a #GRegex
 *
 * Returns the number of capturing subpatterns in the pattern.
 *
 * Returns: the number of capturing subpatterns.
 *
 * Since: 2.14
 */
gint
g_regex_get_capture_count (const GRegex *regex)
{
  gint value;

  pcre_fullinfo (regex->pcre_re, regex->extra,
		 PCRE_INFO_CAPTURECOUNT, &value);

  return value;
}

/**
 * g_regex_match_simple:
 * @pattern: the regular expression
 * @string: the string to scan for matches
 * @compile_options: compile options for the regular expression
 * @match_options: match options
 *
 * Scans for a match in @string for @pattern.
 *
 * This function is equivalent to g_regex_match() but it does not
 * require to compile the pattern with g_regex_new(), avoiding some
 * lines of code when you need just to do a match without extracting
 * substrings, capture counts, and so on.
 *
 * If this function is to be called on the same @pattern more than
 * once, it's more efficient to compile the pattern once with
 * g_regex_new() and then use g_regex_match().
 *
 * Returns: %TRUE is the string matched, %FALSE otherwise
 *
 * Since: 2.14
 */
gboolean
g_regex_match_simple (const gchar        *pattern, 
		      const gchar        *string, 
		      GRegexCompileFlags  compile_options,
		      GRegexMatchFlags    match_options)
{
  GRegex *regex;
  gboolean result;

  regex = g_regex_new (pattern, compile_options, 0, NULL);
  if (!regex)
    return FALSE;
  result = g_regex_match_full (regex, string, -1, 0, match_options, NULL, NULL);
  g_regex_free (regex);
  return result;
}

/**
 * g_regex_match:
 * @regex: a #GRegex structure from g_regex_new()
 * @string: the string to scan for matches
 * @match_options: match options
 * @match_info: pointer to location where to store the #GMatchInfo, or
 * %NULL if you do not need it
 *
 * Scans for a match in string for the pattern in @regex. The @match_options
 * are combined with the match options specified when the @regex structure
 * was created, letting you have more flexibility in reusing #GRegex
 * structures.
 *
 * A #GMatchInfo structure, used to get information on the match, is stored
 * in @match_info if not %NULL.
 *
 * To retrieve all the non-overlapping matches of the pattern in string you
 * can use g_match_info_next().
 *
 * <informalexample><programlisting>
 * static void
 * print_uppercase_words (const gchar *string)
 * {
 *   /&ast; Print all uppercase-only words. &ast;/
 *   GRegex *regex;
 *   GMatchInfo *match_info;
 *   &nbsp;
 *   regex = g_regex_new ("[A-Z]+", 0, 0, NULL);
 *   g_regex_match (regex, string, 0, &match_info);
 *   while (g_match_info_matches (match_info))
 *     {
 *       gchar *word = g_match_info_fetch (match_info, 0);
 *       g_print ("Found: %s\n", word);
 *       g_free (word);
 *       g_match_info_next (match_info, NULL);
 *     }
 *   g_match_info_free (match_info);
 *   g_regex_free (regex);
 * }
 * </programlisting></informalexample>
 *
 * Returns: %TRUE is the string matched, %FALSE otherwise
 *
 * Since: 2.14
 */
gboolean
g_regex_match (const GRegex    *regex, 
	       const gchar     *string, 
	       GRegexMatchFlags match_options,
	       GMatchInfo     **match_info)
{
  return g_regex_match_full (regex, string, -1, 0, match_options,
			     match_info, NULL);
}

/**
 * g_regex_match_full:
 * @regex: a #GRegex structure from g_regex_new()
 * @string: the string to scan for matches
 * @string_len: the length of @string, or -1 if @string is nul-terminated
 * @start_position: starting index of the string to match
 * @match_options: match options
 * @match_info: pointer to location where to store the #GMatchInfo, or
 * %NULL if you do not need it
 * @error: location to store the error occuring, or %NULL to ignore errors
 *
 * Scans for a match in string for the pattern in @regex. The @match_options
 * are combined with the match options specified when the @regex structure
 * was created, letting you have more flexibility in reusing #GRegex
 * structures.
 *
 * Setting @start_position differs from just passing over a shortened string
 * and  setting #G_REGEX_MATCH_NOTBOL in the case of a pattern that begins
 * with any kind of lookbehind assertion, such as "\b".
 *
 * A #GMatchInfo structure, used to get information on the match, is stored
 * in @match_info if not %NULL.
 *
 * To retrieve all the non-overlapping matches of the pattern in string you
 * can use g_match_info_next().
 *
 * <informalexample><programlisting>
 * static void
 * print_uppercase_words (const gchar *string)
 * {
 *   /&ast; Print all uppercase-only words. &ast;/
 *   GRegex *regex;
 *   GMatchInfo *match_info;
 *   GError *error = NULL;
 *   &nbsp;
 *   regex = g_regex_new ("[A-Z]+", 0, 0, NULL);
 *   g_regex_match_full (regex, string, -1, 0, 0, &match_info, &error);
 *   while (g_match_info_matches (match_info))
 *     {
 *       gchar *word = g_match_info_fetch (match_info, 0);
 *       g_print ("Found: %s\n", word);
 *       g_free (word);
 *       g_match_info_next (match_info, &error);
 *     }
 *   g_match_info_free (match_info);
 *   g_regex_free (regex);
 *   if (error != NULL)
 *     {
 *       g_printerr ("Error while matching: %s\n", error->message);
 *       g_error_free (error);
 *     }
 * }
 * </programlisting></informalexample>
 *
 * Returns: %TRUE is the string matched, %FALSE otherwise
 *
 * Since: 2.14
 */
gboolean
g_regex_match_full (const GRegex    *regex,
		    const gchar     *string,
		    gssize           string_len,
		    gint             start_position,
		    GRegexMatchFlags match_options,
		    GMatchInfo     **match_info,
		    GError         **error)
{
  GMatchInfo *info;
  gboolean match_ok;

  g_return_val_if_fail (regex != NULL, FALSE);
  g_return_val_if_fail (string != NULL, FALSE);
  g_return_val_if_fail (start_position >= 0, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail ((match_options & ~G_REGEX_MATCH_MASK) == 0, FALSE);

  info = match_info_new (regex, string, string_len, start_position,
			 match_options, FALSE);
  match_ok = g_match_info_next (info, error);
  if (match_info != NULL)
    *match_info = info;
  else
    g_match_info_free (info);

  return match_ok;
}

/**
 * g_regex_match_all:
 * @regex: a #GRegex structure from g_regex_new()
 * @string: the string to scan for matches
 * @match_options: match options
 * @match_info: pointer to location where to store the #GMatchInfo, or
 * %NULL if you do not need it
 *
 * Using the standard algorithm for regular expression matching only the
 * longest match in the string is retrieved. This function uses a
 * different algorithm so it can retrieve all the possible matches.
 * For more documentation see g_regex_match_all_full().
 *
 * A #GMatchInfo structure, used to get information on the match, is stored
 * in @match_info if not %NULL.
 *
 * Returns: %TRUE is the string matched, %FALSE otherwise
 *
 * Since: 2.14
 */
gboolean
g_regex_match_all (const GRegex    *regex,
		   const gchar     *string,
		   GRegexMatchFlags match_options,
		   GMatchInfo     **match_info)
{
  return g_regex_match_all_full (regex, string, -1, 0, match_options,
				 match_info, NULL);
}

/**
 * g_regex_match_all_full:
 * @regex: a #GRegex structure from g_regex_new()
 * @string: the string to scan for matches
 * @string_len: the length of @string, or -1 if @string is nul-terminated
 * @start_position: starting index of the string to match
 * @match_options: match options
 * @match_info: pointer to location where to store the #GMatchInfo, or
 * %NULL if you do not need it
 * @error: location to store the error occuring, or %NULL to ignore errors
 *
 * Using the standard algorithm for regular expression matching only the
 * longest match in the string is retrieved, it is not possibile to obtain
 * all the available matches. For instance matching
 * "&lt;a&gt; &lt;b&gt; &lt;c&gt;" against the pattern "&lt;.*&gt;" you get
 * "&lt;a&gt; &lt;b&gt; &lt;c&gt;".
 *
 * This function uses a different algorithm (called DFA, i.e. deterministic
 * finite automaton), so it can retrieve all the possible matches, all
 * starting at the same point in the string. For instance matching
 * "&lt;a&gt; &lt;b&gt; &lt;c&gt;" against the pattern "&lt;.*&gt;" you
 * would obtain three matches: "&lt;a&gt; &lt;b&gt; &lt;c&gt;",
 * "&lt;a&gt; &lt;b&gt;" and "&lt;a&gt;".
 *
 * The number of matched strings is retrieved using
 * g_match_info_get_match_count().
 * To obtain the matched strings and their position you can use,
 * respectively, g_match_info_fetch() and g_match_info_fetch_pos(). Note that
 * the strings are returned in reverse order of length; that is, the longest
 * matching string is given first.
 *
 * Note that the DFA algorithm is slower than the standard one and it is not
 * able to capture substrings, so backreferences do not work.
 *
 * Setting @start_position differs from just passing over a shortened string
 * and  setting #G_REGEX_MATCH_NOTBOL in the case of a pattern that begins
 * with any kind of lookbehind assertion, such as "\b".
 *
 * A #GMatchInfo structure, used to get information on the match, is stored
 * in @match_info if not %NULL.
 *
 * Returns: %TRUE is the string matched, %FALSE otherwise
 *
 * Since: 2.14
 */
gboolean
g_regex_match_all_full (const GRegex    *regex,
			const gchar     *string,
			gssize           string_len,
			gint             start_position,
			GRegexMatchFlags match_options,
			GMatchInfo     **match_info,
			GError         **error)
{
  GMatchInfo *info;
  gboolean done;

  g_return_val_if_fail (regex != NULL, FALSE);
  g_return_val_if_fail (string != NULL, FALSE);
  g_return_val_if_fail (start_position >= 0, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail ((match_options & ~G_REGEX_MATCH_MASK) == 0, FALSE);

  info = match_info_new (regex, string, string_len, start_position,
			 match_options, TRUE);

  done = FALSE;
  while (!done)
    {
      done = TRUE;
      info->matches = pcre_dfa_exec (regex->pcre_re, regex->extra,
				     info->string, info->string_len,
				     info->pos,
				     regex->match_opts | match_options,
				     info->offsets, info->n_offsets,
				     info->workspace, info->n_workspace);
      if (info->matches == PCRE_ERROR_DFA_WSSIZE)
	{
	  /* info->workspace is too small. */
	  info->n_workspace *= 2;
	  info->workspace = g_realloc (info->workspace,
				       info->n_workspace * sizeof (gint));
	  done = FALSE;
	}
      else if (info->matches == 0)
	{
	  /* info->offsets is too small. */
	  info->n_offsets *= 2;
	  info->offsets = g_realloc (info->offsets,
				     info->n_offsets * sizeof (gint));
	  done = FALSE;
	}
      else if (IS_PCRE_ERROR (info->matches))
	{
	  g_set_error (error, G_REGEX_ERROR, G_REGEX_ERROR_MATCH,
		       _("Error while matching regular expression %s: %s"),
		       regex->pattern, match_error (info->matches));
	}
    }

  /* set info->pos to -1 so that a call to g_match_info_next() fails. */
  info->pos = -1;

  if (match_info != NULL)
    *match_info = info;
  else
    g_match_info_free (info);

  return info->matches >= 0;
}

/**
 * g_regex_get_string_number:
 * @regex: #GRegex structure
 * @name: name of the subexpression
 *
 * Retrieves the number of the subexpression named @name.
 *
 * Returns: The number of the subexpression or -1 if @name does not exists
 *
 * Since: 2.14
 */
gint
g_regex_get_string_number (const GRegex *regex,
			   const gchar  *name)
{
  gint num;

  g_return_val_if_fail (regex != NULL, -1);
  g_return_val_if_fail (name != NULL, -1);

  num = pcre_get_stringnumber (regex->pcre_re, name);
  if (num == PCRE_ERROR_NOSUBSTRING)
    num = -1;

  return num;
}

/**
 * g_regex_split_simple:
 * @pattern: the regular expression
 * @string: the string to scan for matches
 * @compile_options: compile options for the regular expression
 * @match_options: match options
 *
 * Breaks the string on the pattern, and returns an array of the tokens.
 * If the pattern contains capturing parentheses, then the text for each
 * of the substrings will also be returned. If the pattern does not match
 * anywhere in the string, then the whole string is returned as the first
 * token.
 *
 * This function is equivalent to g_regex_split() but it does not
 * require to compile the pattern with g_regex_new(), avoiding some
 * lines of code when you need just to do a split without extracting
 * substrings, capture counts, and so on.
 *
 * If this function is to be called on the same @pattern more than
 * once, it's more efficient to compile the pattern once with
 * g_regex_new() and then use g_regex_split().
 *
 * As a special case, the result of splitting the empty string "" is an
 * empty vector, not a vector containing a single string. The reason for
 * this special case is that being able to represent a empty vector is
 * typically more useful than consistent handling of empty elements. If
 * you do need to represent empty elements, you'll need to check for the
 * empty string before calling this function.
 *
 * A pattern that can match empty strings splits @string into separate
 * characters wherever it matches the empty string between characters.
 * For example splitting "ab c" using as a separator "\s*", you will get
 * "a", "b" and "c".
 *
 * Returns: a %NULL-terminated gchar ** array. Free it using g_strfreev()
 *
 * Since: 2.14
 **/
gchar **
g_regex_split_simple (const gchar        *pattern,
		      const gchar        *string, 
		      GRegexCompileFlags  compile_options,
		      GRegexMatchFlags    match_options)
{
  GRegex *regex;
  gchar **result;

  regex = g_regex_new (pattern, compile_options, 0, NULL);
  if (!regex)
    return NULL;
  result = g_regex_split_full (regex, string, -1, 0, match_options, 0, NULL);
  g_regex_free (regex);
  return result;
}

/**
 * g_regex_split:
 * @regex: a #GRegex structure
 * @string: the string to split with the pattern
 * @match_options: match time option flags
 *
 * Breaks the string on the pattern, and returns an array of the tokens.
 * If the pattern contains capturing parentheses, then the text for each
 * of the substrings will also be returned. If the pattern does not match
 * anywhere in the string, then the whole string is returned as the first
 * token.
 *
 * As a special case, the result of splitting the empty string "" is an
 * empty vector, not a vector containing a single string. The reason for
 * this special case is that being able to represent a empty vector is
 * typically more useful than consistent handling of empty elements. If
 * you do need to represent empty elements, you'll need to check for the
 * empty string before calling this function.
 *
 * A pattern that can match empty strings splits @string into separate
 * characters wherever it matches the empty string between characters.
 * For example splitting "ab c" using as a separator "\s*", you will get
 * "a", "b" and "c".
 *
 * Returns: a %NULL-terminated gchar ** array. Free it using g_strfreev()
 *
 * Since: 2.14
 **/
gchar **
g_regex_split (const GRegex     *regex, 
	       const gchar      *string, 
	       GRegexMatchFlags  match_options)
{
  return g_regex_split_full (regex, string, -1, 0,
                             match_options, 0, NULL);
}

/**
 * g_regex_split_full:
 * @regex: a #GRegex structure
 * @string: the string to split with the pattern
 * @string_len: the length of @string, or -1 if @string is nul-terminated
 * @start_position: starting index of the string to match
 * @match_options: match time option flags
 * @max_tokens: the maximum number of tokens to split @string into. If this
 *    is less than 1, the string is split completely
 * @error: return location for a #GError
 *
 * Breaks the string on the pattern, and returns an array of the tokens.
 * If the pattern contains capturing parentheses, then the text for each
 * of the substrings will also be returned. If the pattern does not match
 * anywhere in the string, then the whole string is returned as the first
 * token.
 *
 * As a special case, the result of splitting the empty string "" is an
 * empty vector, not a vector containing a single string. The reason for
 * this special case is that being able to represent a empty vector is
 * typically more useful than consistent handling of empty elements. If
 * you do need to represent empty elements, you'll need to check for the
 * empty string before calling this function.
 *
 * A pattern that can match empty strings splits @string into separate
 * characters wherever it matches the empty string between characters.
 * For example splitting "ab c" using as a separator "\s*", you will get
 * "a", "b" and "c".
 *
 * Setting @start_position differs from just passing over a shortened string
 * and  setting #G_REGEX_MATCH_NOTBOL in the case of a pattern that begins
 * with any kind of lookbehind assertion, such as "\b".
 *
 * Returns: a %NULL-terminated gchar ** array. Free it using g_strfreev()
 *
 * Since: 2.14
 **/
gchar **
g_regex_split_full (const GRegex     *regex, 
		    const gchar      *string, 
		    gssize            string_len,
		    gint              start_position,
		    GRegexMatchFlags  match_options,
		    gint              max_tokens,
		    GError          **error)
{
  GError *tmp_error = NULL;
  GMatchInfo *match_info;
  GList *list, *last;
  gint i;
  gint token_count;
  gboolean match_ok;
  /* position of the last separator. */
  gint last_separator_end;
  /* was the last match 0 bytes long? */
  gboolean last_match_is_empty;
  /* the returned array of char **s */
  gchar **string_list;

  g_return_val_if_fail (regex != NULL, NULL);
  g_return_val_if_fail (string != NULL, NULL);
  g_return_val_if_fail (start_position >= 0, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);
  g_return_val_if_fail ((match_options & ~G_REGEX_MATCH_MASK) == 0, NULL);

  if (max_tokens <= 0)
    max_tokens = G_MAXINT;

  if (string_len < 0)
    string_len = strlen (string);

  /* zero-length string */
  if (string_len - start_position == 0)
    return g_new0 (gchar *, 1);

  if (max_tokens == 1)
    {
      string_list = g_new0 (gchar *, 2);
      string_list[0] = g_strndup (&string[start_position],
				  string_len - start_position);
      return string_list;
    }

  list = NULL;
  token_count = 0;
  last_separator_end = start_position;
  last_match_is_empty = FALSE;

  match_ok = g_regex_match_full (regex, string, string_len, start_position,
				 match_options, &match_info, &tmp_error);
  while (tmp_error == NULL)
    {
      if (match_ok)
        {
          last_match_is_empty =
                    (match_info->offsets[0] == match_info->offsets[1]);

          /* we need to skip empty separators at the same position of the end
           * of another separator. e.g. the string is "a b" and the separator
           * is " *", so from 1 to 2 we have a match and at position 2 we have
           * an empty match. */
          if (last_separator_end != match_info->offsets[1])
            {
              gchar *token;
              gint match_count;

              token = g_strndup (string + last_separator_end,
				 match_info->offsets[0] - last_separator_end);
              list = g_list_prepend (list, token);
              token_count++;

              /* if there were substrings, these need to be added to
               * the list. */
              match_count = g_match_info_get_match_count (match_info);
              if (match_count > 1)
                {
                  for (i = 1; i < match_count; i++)
                    list = g_list_prepend (list, g_match_info_fetch (match_info, i));
                }
            }
        }
      else
        {
          /* if there was no match, copy to end of string. */
          if (!last_match_is_empty)
            {
              gchar *token = g_strndup (string + last_separator_end,
					match_info->string_len - last_separator_end);
              list = g_list_prepend (list, token);
            }
          /* no more tokens, end the loop. */
          break;
        }

      /* -1 to leave room for the last part. */
      if (token_count >= max_tokens - 1)
	{
	  /* we have reached the maximum number of tokens, so we copy
	   * the remaining part of the string. */
	  if (last_match_is_empty)
	    {
	      /* the last match was empty, so we have moved one char
	       * after the real position to avoid empty matches at the
	       * same position. */
	      match_info->pos = PREV_CHAR (regex, &string[match_info->pos]) - string;
	    }
	  /* the if is needed in the case we have terminated the available
	   * tokens, but we are at the end of the string, so there are no
	   * characters left to copy. */
	  if (string_len > match_info->pos)
	    {
	      gchar *token = g_strndup (string + match_info->pos,
					string_len - match_info->pos);
	      list = g_list_prepend (list, token);
	    }
	  /* end the loop. */
	  break;
	}

      last_separator_end = match_info->pos;
      if (last_match_is_empty)
        /* if the last match was empty, g_match_info_next() has moved
         * forward to avoid infinite loops, but we still need to copy that
         * character. */
        last_separator_end = PREV_CHAR (regex, &string[last_separator_end]) - string;

      match_ok = g_match_info_next (match_info, &tmp_error);
    }
  g_match_info_free (match_info);
  if (tmp_error != NULL)
    {
      g_propagate_error (error, tmp_error);
      g_list_foreach (list, (GFunc)g_free, NULL);
      g_list_free (list);
      match_info->pos = -1;
      return NULL;
    }

  string_list = g_new (gchar *, g_list_length (list) + 1);
  i = 0;
  for (last = g_list_last (list); last; last = g_list_previous (last))
    string_list[i++] = last->data;
  string_list[i] = 0;
  g_list_free (list);

  return string_list;
}

enum
{
  REPL_TYPE_STRING,
  REPL_TYPE_CHARACTER,
  REPL_TYPE_SYMBOLIC_REFERENCE,
  REPL_TYPE_NUMERIC_REFERENCE,
  REPL_TYPE_CHANGE_CASE
}; 

typedef enum
{
  CHANGE_CASE_NONE         = 1 << 0,
  CHANGE_CASE_UPPER        = 1 << 1,
  CHANGE_CASE_LOWER        = 1 << 2,
  CHANGE_CASE_UPPER_SINGLE = 1 << 3,
  CHANGE_CASE_LOWER_SINGLE = 1 << 4,
  CHANGE_CASE_SINGLE_MASK  = CHANGE_CASE_UPPER_SINGLE | CHANGE_CASE_LOWER_SINGLE,
  CHANGE_CASE_LOWER_MASK   = CHANGE_CASE_LOWER | CHANGE_CASE_LOWER_SINGLE,
  CHANGE_CASE_UPPER_MASK   = CHANGE_CASE_UPPER | CHANGE_CASE_UPPER_SINGLE
} ChangeCase;

struct _InterpolationData
{
  gchar     *text;   
  gint       type;   
  gint       num;
  gchar      c;
  ChangeCase change_case;
};

static void
free_interpolation_data (InterpolationData *data)
{
  g_free (data->text);
  g_free (data);
}

static const gchar *
expand_escape (const gchar        *replacement,
	       const gchar        *p, 
	       InterpolationData  *data,
	       GError            **error)
{
  const gchar *q, *r;
  gint x, d, h, i;
  const gchar *error_detail;
  gint base = 0;
  GError *tmp_error = NULL;

  p++;
  switch (*p)
    {
    case 't':
      p++;
      data->c = '\t';
      data->type = REPL_TYPE_CHARACTER;
      break;
    case 'n':
      p++;
      data->c = '\n';
      data->type = REPL_TYPE_CHARACTER;
      break;
    case 'v':
      p++;
      data->c = '\v';
      data->type = REPL_TYPE_CHARACTER;
      break;
    case 'r':
      p++;
      data->c = '\r';
      data->type = REPL_TYPE_CHARACTER;
      break;
    case 'f':
      p++;
      data->c = '\f';
      data->type = REPL_TYPE_CHARACTER;
      break;
    case 'a':
      p++;
      data->c = '\a';
      data->type = REPL_TYPE_CHARACTER;
      break;
    case 'b':
      p++;
      data->c = '\b';
      data->type = REPL_TYPE_CHARACTER;
      break;
    case '\\':
      p++;
      data->c = '\\';
      data->type = REPL_TYPE_CHARACTER;
      break;
    case 'x':
      p++;
      x = 0;
      if (*p == '{')
	{
	  p++;
	  do 
	    {
	      h = g_ascii_xdigit_value (*p);
	      if (h < 0)
		{
		  error_detail = _("hexadecimal digit or '}' expected");
		  goto error;
		}
	      x = x * 16 + h;
	      p++;
	    }
	  while (*p != '}');
	  p++;
	}
      else
	{
	  for (i = 0; i < 2; i++)
	    {
	      h = g_ascii_xdigit_value (*p);
	      if (h < 0)
		{
		  error_detail = _("hexadecimal digit expected");
		  goto error;
		}
	      x = x * 16 + h;
	      p++;
	    }
	}
      data->type = REPL_TYPE_STRING;
      data->text = g_new0 (gchar, 8);
      g_unichar_to_utf8 (x, data->text);
      break;
    case 'l':
      p++;
      data->type = REPL_TYPE_CHANGE_CASE;
      data->change_case = CHANGE_CASE_LOWER_SINGLE;
      break;
    case 'u':
      p++;
      data->type = REPL_TYPE_CHANGE_CASE;
      data->change_case = CHANGE_CASE_UPPER_SINGLE;
      break;
    case 'L':
      p++;
      data->type = REPL_TYPE_CHANGE_CASE;
      data->change_case = CHANGE_CASE_LOWER;
      break;
    case 'U':
      p++;
      data->type = REPL_TYPE_CHANGE_CASE;
      data->change_case = CHANGE_CASE_UPPER;
      break;
    case 'E':
      p++;
      data->type = REPL_TYPE_CHANGE_CASE;
      data->change_case = CHANGE_CASE_NONE;
      break;
    case 'g':
      p++;
      if (*p != '<')
	{
	  error_detail = _("missing '<' in symbolic reference");
	  goto error;
	}
      q = p + 1;
      do 
	{
	  p++;
	  if (!*p)
	    {
	      error_detail = _("unfinished symbolic reference");
	      goto error;
	    }
	}
      while (*p != '>');
      if (p - q == 0)
	{
	  error_detail = _("zero-length symbolic reference");
	  goto error;
	}
      if (g_ascii_isdigit (*q))
	{
	  x = 0;
	  do 
	    {
	      h = g_ascii_digit_value (*q);
	      if (h < 0)
		{
		  error_detail = _("digit expected");
		  p = q;
		  goto error;
		}
	      x = x * 10 + h;
	      q++;
	    }
	  while (q != p);
	  data->num = x;
	  data->type = REPL_TYPE_NUMERIC_REFERENCE;
	}
      else
	{
	  r = q;
	  do 
	    {
	      if (!g_ascii_isalnum (*r))
		{
		  error_detail = _("illegal symbolic reference");
		  p = r;
		  goto error;
		}
	      r++;
	    }
	  while (r != p);
	  data->text = g_strndup (q, p - q);
	  data->type = REPL_TYPE_SYMBOLIC_REFERENCE;
	}
      p++;
      break;
    case '0':
      /* if \0 is followed by a number is an octal number representing a
       * character, else it is a numeric reference. */
      if (g_ascii_digit_value (*g_utf8_next_char (p)) >= 0)
        {
          base = 8;
          p = g_utf8_next_char (p);
        }
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
      x = 0;
      d = 0;
      for (i = 0; i < 3; i++)
	{
	  h = g_ascii_digit_value (*p);
	  if (h < 0) 
	    break;
	  if (h > 7)
	    {
	      if (base == 8)
		break;
	      else 
		base = 10;
	    }
	  if (i == 2 && base == 10)
	    break;
	  x = x * 8 + h;
	  d = d * 10 + h;
	  p++;
	}
      if (base == 8 || i == 3)
	{
	  data->type = REPL_TYPE_STRING;
	  data->text = g_new0 (gchar, 8);
	  g_unichar_to_utf8 (x, data->text);
	}
      else
	{
	  data->type = REPL_TYPE_NUMERIC_REFERENCE;
	  data->num = d;
	}
      break;
    case 0:
      error_detail = _("stray final '\\'");
      goto error;
      break;
    default:
      error_detail = _("unknown escape sequence");
      goto error;
    }

  return p;

 error:
  /* G_GSSIZE_FORMAT doesn't work with gettext, so we use %lu */
  tmp_error = g_error_new (G_REGEX_ERROR, 
			   G_REGEX_ERROR_REPLACE,
			   _("Error while parsing replacement "
			     "text \"%s\" at char %lu: %s"),
			   replacement, 
			   (gulong)(p - replacement),
			   error_detail);
  g_propagate_error (error, tmp_error);

  return NULL;
}

static GList *
split_replacement (const gchar  *replacement,
		   GError      **error)
{
  GList *list = NULL;
  InterpolationData *data;
  const gchar *p, *start;
  
  start = p = replacement; 
  while (*p)
    {
      if (*p == '\\')
	{
	  data = g_new0 (InterpolationData, 1);
	  start = p = expand_escape (replacement, p, data, error);
	  if (p == NULL)
	    {
	      g_list_foreach (list, (GFunc)free_interpolation_data, NULL);
	      g_list_free (list);
	      free_interpolation_data (data);

	      return NULL;
	    }
	  list = g_list_prepend (list, data);
	}
      else
	{
	  p++;
	  if (*p == '\\' || *p == '\0')
	    {
	      if (p - start > 0)
		{
		  data = g_new0 (InterpolationData, 1);
		  data->text = g_strndup (start, p - start);
		  data->type = REPL_TYPE_STRING;
		  list = g_list_prepend (list, data);
		}
	    }
	}
    }

  return g_list_reverse (list);
}

/* Change the case of c based on change_case. */
#define CHANGE_CASE(c, change_case) \
	(((change_case) & CHANGE_CASE_LOWER_MASK) ? \
		g_unichar_tolower (c) : \
		g_unichar_toupper (c))

static void
string_append (GString     *string,
	       const gchar *text,
               ChangeCase  *change_case)
{
  gunichar c;

  if (text[0] == '\0')
    return;

  if (*change_case == CHANGE_CASE_NONE)
    {
      g_string_append (string, text);
    }
  else if (*change_case & CHANGE_CASE_SINGLE_MASK)
    {
      c = g_utf8_get_char (text);
      g_string_append_unichar (string, CHANGE_CASE (c, *change_case));
      g_string_append (string, g_utf8_next_char (text));
      *change_case = CHANGE_CASE_NONE;
    }
  else
    {
      while (*text != '\0')
        {
          c = g_utf8_get_char (text);
          g_string_append_unichar (string, CHANGE_CASE (c, *change_case));
          text = g_utf8_next_char (text);
        }
    }
}

static gboolean
interpolate_replacement (const GRegex     *regex,
			 const GMatchInfo *match_info,
			 const gchar      *string,
			 GString          *result,
			 gpointer          data)
{
  GList *list;
  InterpolationData *idata;
  gchar *match;
  ChangeCase change_case = CHANGE_CASE_NONE;

  for (list = data; list; list = list->next)
    {
      idata = list->data;
      switch (idata->type)
	{
	case REPL_TYPE_STRING:
	  string_append (result, idata->text, &change_case);
	  break;
	case REPL_TYPE_CHARACTER:
	  g_string_append_c (result, CHANGE_CASE (idata->c, change_case));
          if (change_case & CHANGE_CASE_SINGLE_MASK)
            change_case = CHANGE_CASE_NONE;
	  break;
	case REPL_TYPE_NUMERIC_REFERENCE:
	  match = g_match_info_fetch (match_info, idata->num);
	  if (match)
	    {
	      string_append (result, match, &change_case);
	      g_free (match);
	    }
	  break;
	case REPL_TYPE_SYMBOLIC_REFERENCE:
	  match = g_match_info_fetch_named (match_info, idata->text);
	  if (match)
	    {
	      string_append (result, match, &change_case);
	      g_free (match);
	    }
	  break;
	case REPL_TYPE_CHANGE_CASE:
	  change_case = idata->change_case;
	  break;
	}
    }

  return FALSE; 
}

/**
 * g_regex_replace:
 * @regex: a #GRegex structure
 * @string: the string to perform matches against
 * @string_len: the length of @string, or -1 if @string is nul-terminated
 * @start_position: starting index of the string to match
 * @replacement: text to replace each match with
 * @match_options: options for the match
 * @error: location to store the error occuring, or %NULL to ignore errors
 *
 * Replaces all occurances of the pattern in @regex with the
 * replacement text. Backreferences of the form '\number' or '\g&lt;number&gt;'
 * in the replacement text are interpolated by the number-th captured
 * subexpression of the match, '\g&lt;name&gt;' refers to the captured subexpression
 * with the given name. '\0' refers to the complete match, but '\0' followed
 * by a number is the octal representation of a character. To include a
 * literal '\' in the replacement, write '\\'.
 * There are also escapes that changes the case of the following text:
 *
 * <variablelist>
 * <varlistentry><term>\l</term>
 * <listitem>
 * <para>Convert to lower case the next character</para>
 * </listitem>
 * </varlistentry>
 * <varlistentry><term>\u</term>
 * <listitem>
 * <para>Convert to upper case the next character</para>
 * </listitem>
 * </varlistentry>
 * <varlistentry><term>\L</term>
 * <listitem>
 * <para>Convert to lower case till \E</para>
 * </listitem>
 * </varlistentry>
 * <varlistentry><term>\U</term>
 * <listitem>
 * <para>Convert to upper case till \E</para>
 * </listitem>
 * </varlistentry>
 * <varlistentry><term>\E</term>
 * <listitem>
 * <para>End case modification</para>
 * </listitem>
 * </varlistentry>
 * </variablelist>
 *
 * If you do not need to use backreferences use g_regex_replace_literal().
 *
 * The @replacement string must be UTF-8 encoded even if #G_REGEX_RAW was
 * passed to g_regex_new(). If you want to use not UTF-8 encoded stings
 * you can use g_regex_replace_literal().
 *
 * Setting @start_position differs from just passing over a shortened string
 * and  setting #G_REGEX_MATCH_NOTBOL in the case of a pattern that begins
 * with any kind of lookbehind assertion, such as "\b".
 *
 * Returns: a newly allocated string containing the replacements
 *
 * Since: 2.14
 */
gchar *
g_regex_replace (const GRegex      *regex, 
		 const gchar       *string, 
		 gssize             string_len,
		 gint               start_position,
		 const gchar       *replacement,
		 GRegexMatchFlags   match_options,
		 GError           **error)
{
  gchar *result;
  GList *list;
  GError *tmp_error = NULL;

  g_return_val_if_fail (regex != NULL, NULL);
  g_return_val_if_fail (string != NULL, NULL);
  g_return_val_if_fail (start_position >= 0, NULL);
  g_return_val_if_fail (replacement != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);
  g_return_val_if_fail ((match_options & ~G_REGEX_MATCH_MASK) == 0, NULL);

  list = split_replacement (replacement, &tmp_error);
  if (tmp_error != NULL)
    {
      g_propagate_error (error, tmp_error);
      return NULL;
    }

  result = g_regex_replace_eval (regex, 
				 string, string_len, start_position,
				 match_options,
				 interpolate_replacement,
				 (gpointer)list,
                                 &tmp_error);
  if (tmp_error != NULL)
    g_propagate_error (error, tmp_error);

  g_list_foreach (list, (GFunc)free_interpolation_data, NULL);
  g_list_free (list);

  return result;
}

static gboolean
literal_replacement (const GRegex     *regex,
		     const GMatchInfo *match_info,
		     const gchar      *string,
		     GString          *result,
		     gpointer          data)
{
  g_string_append (result, data);
  return FALSE;
}

/**
 * g_regex_replace_literal:
 * @regex: a #GRegex structure
 * @string: the string to perform matches against
 * @string_len: the length of @string, or -1 if @string is nul-terminated
 * @start_position: starting index of the string to match
 * @replacement: text to replace each match with
 * @match_options: options for the match
 * @error: location to store the error occuring, or %NULL to ignore errors
 *
 * Replaces all occurances of the pattern in @regex with the
 * replacement text. @replacement is replaced literally, to
 * include backreferences use g_regex_replace().
 *
 * Setting @start_position differs from just passing over a shortened string
 * and  setting #G_REGEX_MATCH_NOTBOL in the case of a pattern that begins
 * with any kind of lookbehind assertion, such as "\b".
 *
 * Returns: a newly allocated string containing the replacements
 *
 * Since: 2.14
 */
gchar *
g_regex_replace_literal (const GRegex    *regex,
			 const gchar     *string,
			 gssize           string_len,
			 gint             start_position,
			 const gchar     *replacement,
			 GRegexMatchFlags match_options,
			 GError         **error)
{
  g_return_val_if_fail (replacement != NULL, NULL);
  g_return_val_if_fail ((match_options & ~G_REGEX_MATCH_MASK) == 0, NULL);

  return g_regex_replace_eval (regex,
			       string, string_len, start_position,
			       match_options,
			       literal_replacement,
			       (gpointer)replacement,
			       error);
}

/**
 * g_regex_replace_eval:
 * @regex: a #GRegex structure from g_regex_new()
 * @string: string to perform matches against
 * @string_len: the length of @string, or -1 if @string is nul-terminated
 * @start_position: starting index of the string to match
 * @match_options: options for the match
 * @eval: a function to call for each match
 * @user_data: user data to pass to the function
 * @error: location to store the error occuring, or %NULL to ignore errors
 *
 * Replaces occurances of the pattern in regex with the output of @eval
 * for that occurance.
 *
 * Setting @start_position differs from just passing over a shortened string
 * and  setting #G_REGEX_MATCH_NOTBOL in the case of a pattern that begins
 * with any kind of lookbehind assertion, such as "\b".
 *
 * Returns: a newly allocated string containing the replacements
 *
 * Since: 2.14
 */
gchar *
g_regex_replace_eval (const GRegex      *regex,
		      const gchar       *string,
		      gssize             string_len,
		      gint               start_position,
		      GRegexMatchFlags   match_options,
		      GRegexEvalCallback eval,
		      gpointer           user_data,
		      GError           **error)
{
  GMatchInfo *match_info;
  GString *result;
  gint str_pos = 0;
  gboolean done = FALSE;
  GError *tmp_error = NULL;

  g_return_val_if_fail (regex != NULL, NULL);
  g_return_val_if_fail (string != NULL, NULL);
  g_return_val_if_fail (start_position >= 0, NULL);
  g_return_val_if_fail (eval != NULL, NULL);
  g_return_val_if_fail ((match_options & ~G_REGEX_MATCH_MASK) == 0, NULL);

  if (string_len < 0)
    string_len = strlen (string);

  result = g_string_sized_new (string_len);

  /* run down the string making matches. */
  g_regex_match_full (regex, string, string_len, start_position,
		      match_options, &match_info, &tmp_error);
  while (!done && g_match_info_matches (match_info))
    {
      g_string_append_len (result,
			   string + str_pos,
			   match_info->offsets[0] - str_pos);
      done = (*eval) (regex, match_info, string, result, user_data);
      str_pos = match_info->offsets[1];
      g_match_info_next (match_info, &tmp_error);
    }
  g_match_info_free (match_info);
  if (tmp_error != NULL)
    {
      g_propagate_error (error, tmp_error);
      g_string_free (result, TRUE);
      return NULL;
    }

  g_string_append_len (result, string + str_pos, string_len - str_pos);
  return g_string_free (result, FALSE);
}

/**
 * g_regex_escape_string:
 * @string: the string to escape
 * @length: the length of @string, or -1 if @string is nul-terminated
 *
 * Escapes the special characters used for regular expressions in @string,
 * for instance "a.b*c" becomes "a\.b\*c". This function is useful to
 * dynamically generate regular expressions.
 *
 * @string can contain nul characters that are replaced with "\0", in this
 * case remember to specify the correct length of @string in @length.
 *
 * Returns: a newly-allocated escaped string
 *
 * Since: 2.14
 */
gchar *
g_regex_escape_string (const gchar *string,
		       gint         length)
{
  GString *escaped;
  const char *p, *piece_start, *end;

  g_return_val_if_fail (string != NULL, NULL);

  if (length < 0)
    length = strlen (string);

  end = string + length;
  p = piece_start = string;
  escaped = g_string_sized_new (length + 1);

  while (p < end)
    {
      switch (*p)
	{
        case '\0':
	case '\\':
	case '|':
	case '(':
	case ')':
	case '[':
	case ']':
	case '{':
	case '}':
	case '^':
	case '$':
	case '*':
	case '+':
	case '?':
	case '.':
	  if (p != piece_start)
	    /* copy the previous piece. */
	    g_string_append_len (escaped, piece_start, p - piece_start);
	  g_string_append_c (escaped, '\\');
          if (*p == '\0')
            g_string_append_c (escaped, '0');
          else
	    g_string_append_c (escaped, *p);
	  piece_start = ++p;
	  break;
	default:
	  p = g_utf8_next_char (p);
          break;
        } 
  }

  if (piece_start < end)
    g_string_append_len (escaped, piece_start, end - piece_start);

  return g_string_free (escaped, FALSE);
}

#define __G_REGEX_C__
#include "galiasdef.c"