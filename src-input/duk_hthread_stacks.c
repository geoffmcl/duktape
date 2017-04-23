/*
 *  Thread stack (mainly call stack) primitives: allocation of activations,
 *  unwinding catchers and activations, etc.
 *
 *  Value stack handling is a part of the API implementation.
 */

#include "duk_internal.h"

/* Unwind the topmost catcher of the current activation (caller must check that
 * both exist) without side effects.
 */
DUK_INTERNAL void duk_hthread_catcher_unwind_norz(duk_hthread *thr, duk_activation *act) {
	duk_catcher *cat;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(act != NULL);
	DUK_ASSERT(act->cat != NULL);  /* caller must check */
	cat = act->cat;
	DUK_ASSERT(cat != NULL);

	DUK_DDD(DUK_DDDPRINT("unwinding catch stack entry %p (lexenv check is done)", (void *) cat));

	if (DUK_CAT_HAS_LEXENV_ACTIVE(cat)) {
		duk_hobject *env;

		env = act->lex_env;             /* current lex_env of the activation (created for catcher) */
		DUK_ASSERT(env != NULL);        /* must be, since env was created when catcher was created */
		act->lex_env = DUK_HOBJECT_GET_PROTOTYPE(thr->heap, env);  /* prototype is lex_env before catcher created */
		DUK_HOBJECT_INCREF(thr, act->lex_env);
		DUK_HOBJECT_DECREF_NORZ(thr, env);

		/* There is no need to decref anything else than 'env': if 'env'
		 * becomes unreachable, refzero will handle decref'ing its prototype.
		 */
	}

	act->cat = cat->parent;
	duk_hthread_catcher_free(thr, cat);
}

/* Same as above, but caller is certain no catcher-related lexenv may exist. */
DUK_INTERNAL void duk_hthread_catcher_unwind_nolexenv_norz(duk_hthread *thr, duk_activation *act) {
	duk_catcher *cat;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(act != NULL);
	DUK_ASSERT(act->cat != NULL);  /* caller must check */
	cat = act->cat;
	DUK_ASSERT(cat != NULL);

	DUK_DDD(DUK_DDDPRINT("unwinding catch stack entry %p (lexenv check is not done)", (void *) cat));

	DUK_ASSERT(!DUK_CAT_HAS_LEXENV_ACTIVE(cat));

	act->cat = cat->parent;
	duk_hthread_catcher_free(thr, cat);
}

DUK_INTERNAL duk_catcher *duk_hthread_catcher_alloc(duk_hthread *thr) {
	duk_catcher *cat;

	DUK_ASSERT(thr != NULL);

	cat = (duk_catcher *) DUK_ALLOC_CHECKED(thr, sizeof(duk_catcher));
	DUK_ASSERT(cat != NULL);
	return cat;
}

DUK_INTERNAL void duk_hthread_catcher_free(duk_hthread *thr, duk_catcher *cat) {
	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(cat != NULL);

	DUK_FREE_CHECKED(thr, (void *) cat);
}

DUK_INTERNAL duk_activation *duk_hthread_activation_alloc(duk_hthread *thr) {
	duk_activation *act;

	DUK_ASSERT(thr != NULL);

#if 0
	/* XXX: inline the check part? */
	act = thr->heap->activation_free;
	if (DUK_LIKELY(act != NULL)) {
		thr->heap->activation_free = act->parent;
		return act;
	}
#endif

	act = (duk_activation *) DUK_ALLOC_CHECKED(thr, sizeof(duk_activation));
	DUK_ASSERT(act != NULL);
	return act;
}

DUK_INTERNAL void duk_hthread_activation_free(duk_hthread *thr, duk_activation *act) {
	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(act != NULL);

#if 0
	act->parent = thr->heap->activation_free;
	thr->heap->activation_free = act;
#else
	DUK_FREE_CHECKED(thr, (void *) act);
#endif
}

/* Internal helper: process the unwind for the topmost activation of a thread,
 * but leave the duk_activation in place for possible tailcall reuse.
 */
DUK_LOCAL void duk__activation_unwind_nofree_norz(duk_hthread *thr) {
#if defined(DUK_USE_DEBUGGER_SUPPORT)
	duk_heap *heap;
#endif
	duk_activation *act;
	duk_hobject *func;
	duk_hobject *tmp;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(thr->callstack_curr != NULL);  /* caller must check */
	DUK_ASSERT(thr->callstack_top > 0);
	act = thr->callstack_curr;
	DUK_ASSERT(act != NULL);
	/* With lightfuncs, act 'func' may be NULL. */

	/* With duk_activation records allocated separately, 'act' is a stable
	 * pointer and not affected by side effects.
	 */

#if defined(DUK_USE_NONSTD_FUNC_CALLER_PROPERTY)
	/*
	 *  Restore 'caller' property for non-strict callee functions.
	 */

	func = DUK_ACT_GET_FUNC(act);
	if (func != NULL && !DUK_HOBJECT_HAS_STRICT(func)) {
		duk_tval *tv_caller;
		duk_tval tv_tmp;
		duk_hobject *h_tmp;

		tv_caller = duk_hobject_find_existing_entry_tval_ptr(thr->heap, func, DUK_HTHREAD_STRING_CALLER(thr));

		/* The act->prev_caller should only be set if the entry for 'caller'
		 * exists (as it is only set in that case, and the property is not
		 * configurable), but handle all the cases anyway.
		 */

		if (tv_caller) {
			DUK_TVAL_SET_TVAL(&tv_tmp, tv_caller);
			if (act->prev_caller) {
				/* Just transfer the refcount from act->prev_caller to tv_caller,
				 * so no need for a refcount update.  This is the expected case.
				 */
				DUK_TVAL_SET_OBJECT(tv_caller, act->prev_caller);
				act->prev_caller = NULL;
			} else {
				DUK_TVAL_SET_NULL(tv_caller);   /* no incref needed */
				DUK_ASSERT(act->prev_caller == NULL);
			}
			DUK_TVAL_DECREF_NORZ(thr, &tv_tmp);
		} else {
			h_tmp = act->prev_caller;
			if (h_tmp) {
				act->prev_caller = NULL;
				DUK_HOBJECT_DECREF_NORZ(thr, h_tmp);
			}
		}
		DUK_ASSERT(act->prev_caller == NULL);
	}
#endif

	/*
	 *  Unwind debugger state.  If we unwind while stepping
	 *  (for any step type), pause execution.  This is the
	 *  only place explicitly handling a step out.
	 */

#if defined(DUK_USE_DEBUGGER_SUPPORT)
	heap = thr->heap;
	if (heap->dbg_step_act == thr->callstack_curr) {
		if (duk_debug_is_paused(heap)) {
			DUK_D(DUK_DPRINT("step pause trigger but already paused, ignoring"));
		} else {
			duk_debug_set_paused(heap);
			DUK_ASSERT(heap->dbg_step_act == NULL);
		}
	}
#endif

	/*
	 *  Unwind catchers.
	 *
	 *  Since there are no references in the catcher structure,
	 *  unwinding is quite simple.  The only thing we need to
	 *  look out for is popping a possible lexical environment
	 *  established for an active catch clause.
	 */

	while (act->cat != NULL) {
		duk_hthread_catcher_unwind_norz(thr, act);
	}

	/*
	 *  Close environment record(s) if they exist.
	 *
	 *  Only variable environments are closed.  If lex_env != var_env, it
	 *  cannot currently contain any register bound declarations.
	 *
	 *  Only environments created for a NEWENV function are closed.  If an
	 *  environment is created for e.g. an eval call, it must not be closed.
	 */

	func = DUK_ACT_GET_FUNC(act);
	if (func != NULL && !DUK_HOBJECT_HAS_NEWENV(func)) {
		DUK_DDD(DUK_DDDPRINT("skip closing environments, envs not owned by this activation"));
		goto skip_env_close;
	}
	/* func is NULL for lightfunc */

	/* Catch sites are required to clean up their environments
	 * in FINALLY part before propagating, so this should
	 * always hold here.
	 */
	DUK_ASSERT(act->lex_env == act->var_env);

	/* XXX: Closing the environment record copies values from registers
	 * into the scope object.  It's side effect free as such, but may
	 * currently run out of memory which causes an error throw.  This is
	 * an actual sandboxing problem for error unwinds, and needs to be
	 * fixed e.g. by preallocating the scope property slots.
	 */
	if (act->var_env != NULL) {
		DUK_DDD(DUK_DDDPRINT("closing var_env record %p -> %!O",
		                     (void *) act->var_env, (duk_heaphdr *) act->var_env));
		duk_js_close_environment_record(thr, act->var_env);
	}

 skip_env_close:

	/*
	 *  Update preventcount
	 */

	if (act->flags & DUK_ACT_FLAG_PREVENT_YIELD) {
		DUK_ASSERT(thr->callstack_preventcount >= 1);
		thr->callstack_preventcount--;
	}

	/*
	 *  Reference count updates, using NORZ macros so we don't
	 *  need to handle side effects.
	 *
	 *  duk_activation pointers like act->var_env are intentionally
	 *  left as garbage and not NULLed.  Without side effects they
	 *  can't be used when the values are dangling/garbage.
	 */

	DUK_HOBJECT_DECREF_NORZ_ALLOWNULL(thr, act->var_env);
	DUK_HOBJECT_DECREF_NORZ_ALLOWNULL(thr, act->lex_env);
	tmp = DUK_ACT_GET_FUNC(act);
	DUK_HOBJECT_DECREF_NORZ_ALLOWNULL(thr, tmp);
	DUK_UNREF(tmp);
}

/* Unwind topmost duk_activation of a thread, caller must ensure that an
 * activation exists.  The call is side effect free, except that scope
 * closure may currently throw an out-of-memory error.
 */
DUK_INTERNAL void duk_hthread_activation_unwind_norz(duk_hthread *thr) {
	duk_activation *act;

	duk__activation_unwind_nofree_norz(thr);

	DUK_ASSERT(thr->callstack_curr != NULL);
	DUK_ASSERT(thr->callstack_top > 0);
	act = thr->callstack_curr;
	thr->callstack_curr = act->parent;
	thr->callstack_top--;

	/* XXX: inline for performance builds? */
	duk_hthread_activation_free(thr, act);

	/* We could clear the book-keeping variables like idx_retval for the
	 * topmost activation, but don't do so now as it's not necessary.
	 */
}

DUK_INTERNAL void duk_hthread_activation_unwind_reuse_norz(duk_hthread *thr) {
	duk__activation_unwind_nofree_norz(thr);
}

/* Get duk_activation for given callstack level or NULL if level is invalid
 * or deeper than the call stack.  Level -1 refers to current activation, -2
 * to its caller, etc.  Starting from Duktape 2.2 finding the activation is
 * a linked list scan which gets more expensive the deeper the lookup is.
 */
DUK_INTERNAL duk_activation *duk_hthread_get_activation_for_level(duk_hthread *thr, duk_int_t level) {
	duk_activation *act;

	if (level >= 0) {
		return NULL;
	}
	act = thr->callstack_curr;
	for (;;) {
		if (act == NULL) {
			return act;
		}
		if (level == -1) {
			return act;
		}
		level++;
		act = act->parent;
	}
	/* never here */
}

#if defined(DUK_USE_FINALIZER_TORTURE)
DUK_INTERNAL void duk_hthread_valstack_torture_realloc(duk_hthread *thr) {
	duk_size_t alloc_size;
	duk_tval *new_ptr;
	duk_ptrdiff_t end_off;
	duk_ptrdiff_t bottom_off;
	duk_ptrdiff_t top_off;

	if (thr->valstack == NULL) {
		return;
	}

	end_off = (duk_ptrdiff_t) ((duk_uint8_t *) thr->valstack_end - (duk_uint8_t *) thr->valstack);
	bottom_off = (duk_ptrdiff_t) ((duk_uint8_t *) thr->valstack_bottom - (duk_uint8_t *) thr->valstack);
	top_off = (duk_ptrdiff_t) ((duk_uint8_t *) thr->valstack_top - (duk_uint8_t *) thr->valstack);
	alloc_size = (duk_size_t) end_off;
	if (alloc_size == 0) {
		return;
	}

	new_ptr = (duk_tval *) DUK_ALLOC(thr->heap, alloc_size);
	if (new_ptr != NULL) {
		DUK_MEMCPY((void *) new_ptr, (const void *) thr->valstack, alloc_size);
		DUK_MEMSET((void *) thr->valstack, 0x55, alloc_size);
		DUK_FREE_CHECKED(thr, (void *) thr->valstack);
		thr->valstack = new_ptr;
		thr->valstack_end = (duk_tval *) ((duk_uint8_t *) new_ptr + end_off);
		thr->valstack_bottom = (duk_tval *) ((duk_uint8_t *) new_ptr + bottom_off);
		thr->valstack_top = (duk_tval *) ((duk_uint8_t *) new_ptr + top_off);
		/* No change in size. */
	} else {
		DUK_D(DUK_DPRINT("failed to realloc valstack for torture, ignore"));
	}
}
#endif  /* DUK_USE_FINALIZER_TORTURE */
