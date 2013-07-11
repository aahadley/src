/* $OpenBSD: fuse_opt.c,v 1.5 2013/07/11 11:41:12 syl Exp $ */
/*
 * Copyright (c) 2013 Sylvestre Gallon <ccna.syl@gmail.com>
 * Copyright (c) 2013 Stefan Sperling <stsp@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "fuse_opt.h"
#include "fuse_private.h"

static void
free_argv(char **argv, int argc)
{
	int i;

	for (i = 0; i < argc; i++)
		free(argv[i]);
	free(argv);
}

static int
alloc_argv(struct fuse_args *args)
{
	char **argv;
	int i;

	assert(!args->allocated);

	argv = calloc(args->argc, sizeof(*argv));
	if (argv == NULL)
		return (-1);

	if (args->argv) {
		for (i = 0; i < args->argc; i++) {
			argv[i] = strdup(args->argv[i]);
			if (argv[i] == NULL) {
				free_argv(argv, i + 1);
				return (-1);
			}
		}
	}

	args->allocated = 1;
	args->argv = argv;

	return (0);
}

static int
match_opt(const char *templ, const char *opt)
{
	const char *o, *t;
	char *arg;

	arg = strpbrk(templ, " =");

	/* verify template */
	t = templ;
	if (*t == '-') {
		t++;
		if (*t == '-')
			t++;
		if (*t == 'o' || t == '\0')
			return (0);
	}

	/* skip leading -, -o, and -- in option name */
	o = opt;
	if (*o == '-') {
		o++;
		if (*o == 'o' || *o == '-')
			o++;
	}

	/* empty option name is invalid */
	if (*o == '\0')
		return (0);

	/* match option name */
	while (*t && *o) {
		if (*t++ != *o++)
			return (0);
		if (arg && t == arg) {
			if (*o != ' ' && *o != '=')
				return (0);
			o++; /* o now points at argument */
			if (*o == '\0')
				return (0);
			break;
		}
	}

	/* match argument */
	if (arg) {
		if (t != arg)
			return (0);
		t++;
		/* does template have an argument? */
		if (*t != '%' && *t != '\0')
			return (0);
		if (*t == '%' && t[1] == '\0')
			return (0);
		/* yes it does, consume argument in opt */
		while (*o && *o != ' ')
			o++;
	} else if (*t != '\0')
		return (0);

	/* we should have consumed entire opt string */
	if (*o != '\0')
		return (0);

	return (1);
}

static int
add_opt(char **opts, const char *opt)
{
	char *new_opts;

	if (*opts == NULL) {
		*opts = strdup(opt);
		if (*opts == NULL)
			return (-1);
		return (0);
	}

	if (asprintf(&new_opts, "%s,%s", *opts, opt) == -1)
		return (-1);

	free(*opts);
	*opts = new_opts;
	return (0);
}

int
fuse_opt_add_opt(char **opts, const char *opt)
{
	int ret;

	if (opt == NULL || opt[0] == '\0')
		return (-1);

	ret = add_opt(opts, opt);
	return (ret);
}

int
fuse_opt_add_opt_escaped(char **opts, const char *opt)
{
	size_t size = 0, escaped = 0;
	const char *s = opt;
	char *escaped_opt, *p;
	int ret;

	if (opt == NULL || opt[0] == '\0')
		return (-1);

	while (*s) {
		/* malloc(size + escaped) overflow check */
		if (size >= (SIZE_T_MAX / 2))
			return (-1);

		if (*s == ',' || *s == '\\')
			escaped++;
		s++;
		size++;
	}

	if (escaped > 0) {
		escaped_opt = malloc(size + escaped);
		if (escaped_opt == NULL)
			return (-1);
		s = opt;
		p = escaped_opt;
		while (*s) {
			switch (*s) {
			case ',':
			case '\\':
				*p++ = '\\';
				/* FALLTHROUGH */
			default:
				*p++ = *s++;
			}
		}
		*p = '\0';
	} else {
		escaped_opt = strdup(opt);
		if (escaped_opt == NULL)
			return (-1);
	}

	ret = add_opt(opts, escaped_opt);
	free(escaped_opt);
	return (ret);
}

int
fuse_opt_add_arg(struct fuse_args *args, const char *name)
{
	return (fuse_opt_insert_arg(args, args->argc, name));
}

int
fuse_opt_parse(struct fuse_args *args, void *data, struct fuse_opt *opt,
    fuse_opt_proc_t f)
{
	const char *arg;
	struct fuse_opt *good;
	int ret;
	int i, j;

	for (i = 0; i < args->argc; i++) {
		arg = args->argv[i];

		/* not - and not -- */
		if (arg[0] != '-') {
			ret = (f) ? f(data, arg, FUSE_OPT_KEY_NONOPT, 0) : 0;

			if (ret == -1)
				return (ret);
		} else {
			switch (arg[1]) {
			case 'o':
				DPRINTF("%s: -o X,Y not supported yet.\n",
				    __func__);
				break ;
			case '-':
				DPRINTF("%s: long option not supported yet.",
				    __func__);
				break ;
			default:
				good = NULL;
				for (j = 0; opt[j].templ != NULL; j++)
					if (strcmp(arg, opt[j].templ) == 0) {
						good = &opt[j];
						break ;
					}

				if (!good)
					break ;

				if (good->val == -1 && f) {
					ret = f(data, arg, good->val, 0);

					if (ret == -1)
						return (ret);
				}
				break;
			}
		}
	}
	return (0);
}

int
fuse_opt_insert_arg(struct fuse_args *args, int p, const char *name)
{
	char **av;
	char *this_arg, *next_arg;
	int i;

	if (name == NULL || name[0] == '\0')
		return (-1);

	if (!args->allocated && alloc_argv(args))
		return (-1);

	if (p < 0 || p > args->argc)
		return (-1);

	av = realloc(args->argv, (args->argc + 1) * sizeof(*av));
	if (av == NULL)
		return (-1);

	this_arg = strdup(name);
	if (this_arg == NULL) {
		free(av);
		return (-1);
	}

	args->argc++;
	args->argv = av;
	for (i = p; i < args->argc; i++) {
		next_arg = args->argv[i];
		args->argv[i] = this_arg;
		this_arg = next_arg;
	}
	return (0);
}

void
fuse_opt_free_args(struct fuse_args *args)
{
	if (!args->allocated)
		return;

	free_argv(args->argv, args->argc);
	args->argv = 0;
	args->argc = 0;
	args->allocated = 0;
}

int
fuse_opt_match(const struct fuse_opt *opts, const char *opt)
{
	const struct fuse_opt *this_opt = opts;

	if (opt == NULL || opt[0] == '\0')
		return (0);

	while (this_opt->templ) {
		if (match_opt(this_opt->templ, opt))
			return (1);
		this_opt++;
	}

	return (0);
}
