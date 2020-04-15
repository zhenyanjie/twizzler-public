#include <fnmatch.h>
#include <libgen.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <twz/fault.h>
#include <twz/twztry.h>

struct test {
	const char *name;
	void (*fn)(struct test *);
	size_t nr_errors;
};

#define TEST_DECL(n, f)                                                                            \
	(struct test)                                                                                  \
	{                                                                                              \
		.name = n, .fn = f, .nr_errors = 0                                                         \
	}

#define TEST_CHECK_EQ(t, a, b)                                                                     \
	({                                                                                             \
		if((a) != (b)) {                                                                           \
			fprintf(stderr,                                                                        \
			  "  \e[1;31mcheck failed \e[0mat \e[1m%s:%d -- EQ " #a " and " #b "\e[0m\n",          \
			  __FILE__,                                                                            \
			  __LINE__);                                                                           \
			t->nr_errors++;                                                                        \
		};                                                                                         \
	})

static void _test_test(struct test *test)
{
	int x = 1;
	int y = 2;

	TEST_CHECK_EQ(test, x, y - 1);
}

static void _foo_bar(struct test *test)
{
	int x = 1;
	int y = 2;

	TEST_CHECK_EQ(test, x, y);

	twztry
	{
		int *p = NULL;
		volatile int z = *p;
	}
	twzcatch(FAULT_NULL)
	{
	}
	twztry_end;
	/*
	    jmp_buf jmp;
	    jmp_buf *pj = _try_buf;
	    _try_buf = &jmp;
	    if(!setjmp(jmp)) {
	    } else {
	        printf("Caught exception\n");
	    }

	    _try_buf = pj;*/
}

struct test tests[] = {
	TEST_DECL("test-test", _test_test),
	TEST_DECL("foo-bar", _foo_bar),
};

static const size_t nr_tests = sizeof(tests) / sizeof(tests[0]);

struct lnode {
	char *str;
	struct lnode *next;
};

void usage()
{
	printf("USAGE: testbench [-a] [-t <test-name-pattern>]...\n");
	printf("flags:\n");
	printf("  -a                           : Run all tests\n");
	printf("  -t <test-name-pattern>       : Run tests that match pattern.\n");
	printf(
	  "Test name patterns are like globs: test-* will match any test that starts with `test-'.\n");
}

static size_t successes = 0, fails = 0, incompletes = 0;

void run_test(struct test *test)
{
	printf("\e[32mSTARTING TEST \e[1;33m%s\e[0m\n", test->name);
	twztry
	{
		test->fn(test);
	}
	twzcatch_all
	{
		printf("  \e[1;31m-- test ended prematurely due to fault\e[0m\n");
		incompletes++;
	}
	twztry_end;
	if(test->nr_errors > 0) {
		printf("  \e[1;31m-- encountered \e[1;37m%ld\e[1;31m errors\e[0m\n", test->nr_errors);
		fails++;
	} else {
		printf("  \e[1;34m-- encountered no errors\e[0m\n");
		successes++;
	}
}

int main(int argc, char **argv)
{
	int c;
	bool all = false;
	struct lnode *list_of_tests = NULL;
	struct lnode *tail = NULL;
	while((c = getopt(argc, argv, "at:h")) != EOF) {
		switch(c) {
			case 'a':
				all = true;
				break;
			case 't': {
				struct lnode *ln = malloc(sizeof(*ln));
				if(tail) {
					tail->next = ln;
				} else {
					list_of_tests = tail = ln;
				}
				ln->next = NULL;
				ln->str = strdup(optarg);
			} break;
			default:
			case 'h':
				usage();
				exit(c == 'h' ? 0 : 1);
				break;
		}
	}

	if(all) {
		for(size_t i = 0; i < nr_tests; i++) {
			struct test *test = &tests[i];
			run_test(test);
		}
	} else {
		for(struct lnode *ln = list_of_tests; ln; ln = ln->next) {
			for(size_t i = 0; i < nr_tests; i++) {
				struct test *test = &tests[i];
				if(!fnmatch(ln->str, test->name, 0)) {
					run_test(test);
				}
			}
		}
	}
	printf("SUMMARY: %ld tests succeeded; %ld tests failed; %ld tests did not complete\n",
	  successes,
	  fails,
	  incompletes);
	return 0;
}
