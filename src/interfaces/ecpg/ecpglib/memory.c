/* src/interfaces/ecpg/ecpglib/memory.c */

#define POSTGRES_ECPG_INTERNAL
#include "postgres_fe.h"

#include "ecpg-pthread-win32.h"
#include "ecpgerrno.h"
#include "ecpglib.h"
#include "ecpglib_extern.h"
#include "ecpgtype.h"

void
ecpg_free(void *ptr)
{
	free(ptr);
}

char *
ecpg_alloc(long size, int lineno)
{
	char	   *new = (char *) calloc(1L, size);

	if (!new)
	{
		ecpg_raise(lineno, ECPG_OUT_OF_MEMORY, ECPG_SQLSTATE_ECPG_OUT_OF_MEMORY, NULL);
		return NULL;
	}

	return new;
}

char *
ecpg_realloc(void *ptr, long size, int lineno)
{
	char	   *new = (char *) realloc(ptr, size);

	if (!new)
	{
		ecpg_raise(lineno, ECPG_OUT_OF_MEMORY, ECPG_SQLSTATE_ECPG_OUT_OF_MEMORY, NULL);
		return NULL;
	}

	return new;
}

char *
ecpg_strdup(const char *string, int lineno)
{
	char	   *new;

	if (string == NULL)
		return NULL;

	new = strdup(string);
	if (!new)
	{
		ecpg_raise(lineno, ECPG_OUT_OF_MEMORY, ECPG_SQLSTATE_ECPG_OUT_OF_MEMORY, NULL);
		return NULL;
	}

	return new;
}

/* keep a list of memory we allocated for the user */
struct auto_mem
{
	union {
		void *pointer;
		uintptr_t auto_clear_disabled:1;
	};
	struct auto_mem *next;
};

#ifdef ENABLE_THREAD_SAFETY
static pthread_key_t auto_mem_key;
static pthread_once_t auto_mem_once = PTHREAD_ONCE_INIT;

static void
auto_mem_destructor(void *arg)
{
	(void) arg;					/* keep the compiler quiet */
	ECPGfree_auto_mem();
}

static void
auto_mem_key_init(void)
{
	pthread_key_create(&auto_mem_key, auto_mem_destructor);
}

static struct auto_mem *
get_auto_allocs(void)
{
	pthread_once(&auto_mem_once, auto_mem_key_init);
	return (struct auto_mem *) pthread_getspecific(auto_mem_key);
}

static void
set_auto_allocs(struct auto_mem *am)
{
	pthread_setspecific(auto_mem_key, am);
}
#else
static struct auto_mem *auto_allocs = NULL;

#define get_auto_allocs()		(auto_allocs)
#define set_auto_allocs(am)		do { auto_allocs = (am); } while(0)
#endif

char *
ecpg_auto_alloc(long size, int lineno)
{
	void	   *ptr = (void *) ecpg_alloc(size, lineno);

	if (!ptr)
		return NULL;

	if (!ecpg_add_mem(ptr, lineno))
	{
		ecpg_free(ptr);
		return NULL;
	}
	return ptr;
}

bool
ecpg_add_mem(void *ptr, int lineno)
{
	struct auto_mem *am = (struct auto_mem *) ecpg_alloc(sizeof(struct auto_mem), lineno);
	bool auto_clear_disabled = false;

	if (!am)
		return false;

	am->pointer = ptr;
	am->next = get_auto_allocs();
	/* remove the marker bit from the previous head and set on this one */
	if (am->next)
	{
		auto_clear_disabled = am->next->auto_clear_disabled;
		am->next->auto_clear_disabled = 0;
	}
	if (auto_clear_disabled)
		am->auto_clear_disabled = 1;

	set_auto_allocs(am);
	return true;
}

void
ECPGfree_auto_mem(void)
{
	struct auto_mem *am = get_auto_allocs();

	/* free all memory we have allocated for the user */
	if (am)
	{
		/* reset auto-clear disable flag as this is either user-initiated or as
		   a result of an error. */
		if (am->auto_clear_disabled)
		{
			ecpg_log("ECPGfree_auto_mem re-enabled auto-clear on exec\n");
			am->auto_clear_disabled = 0;
		}

		do
		{
			struct auto_mem *act = am;

			am = am->next;
			ecpg_free(act->pointer);
			ecpg_free(act);
		} while (am);
		set_auto_allocs(NULL);
	}
}

void
ECPGdisable_auto_mem_clear_on_exec(void)
{
	struct auto_mem *am = get_auto_allocs();

	ecpg_log("Disabling auto-free on exec\n");

	if (am)
	{
		if (am->auto_clear_disabled)
			ecpg_log("warning: Logic error: auto-clear on exec already disabled for this thread\n");
	}
	else
	{
		am = (struct auto_mem *) ecpg_alloc(sizeof(struct auto_mem), 0);
		set_auto_allocs(am);
	}
	am->auto_clear_disabled = 1;
}

void
ecpg_clear_auto_mem(void)
{
	struct auto_mem *am = get_auto_allocs();

	/* only free our own structure */
	if (am)
	{
		if (am->auto_clear_disabled)
		{
			ecpg_log("Not freeing auto-allocated structures; awaiting ECPGfree_auto_mem()\n");
			return;
		}

		do
		{
			struct auto_mem *act = am;

			am = am->next;
			ecpg_free(act);
		} while (am);
		set_auto_allocs(NULL);
	}
}
