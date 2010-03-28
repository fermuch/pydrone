#ifdef UWSGI_UGREEN

#include "uwsgi.h"

#define GREEN_STACK_SIZE 128 * 1024

extern struct uwsgi_server uwsgi;


static int green_blocking(struct uwsgi_server *uwsgi) {
        struct wsgi_request* wsgi_req = uwsgi->wsgi_requests ;
        int i ;

        for(i=0;i<uwsgi->async;i++) {
                if (wsgi_req->async_status != UWSGI_ACCEPTING) {
                        return 0 ;
                }
                wsgi_req = next_wsgi_req(uwsgi, wsgi_req) ;
        }

        return -1 ;
}

static void u_green_schedule_to_main(struct uwsgi_server *uwsgi, int async_id) {

	int py_current_recursion_depth;
	struct _frame* py_current_frame;

	PyThreadState* tstate = PyThreadState_GET();	
	py_current_recursion_depth = tstate->recursion_depth;
	py_current_frame = tstate->frame;

	swapcontext(uwsgi->green_contexts[async_id], &uwsgi->greenmain);

	tstate = PyThreadState_GET();	
	tstate->recursion_depth = py_current_recursion_depth;
	tstate->frame = py_current_frame ;
}

static void u_green_schedule_to_req(struct uwsgi_server *uwsgi, struct wsgi_request *wsgi_req) {

	int py_current_recursion_depth;
	struct _frame* py_current_frame;

	PyThreadState* tstate = PyThreadState_GET();	
	py_current_recursion_depth = tstate->recursion_depth;
	py_current_frame = tstate->frame;

	uwsgi->wsgi_req = wsgi_req;
	wsgi_req->async_switches++;
	swapcontext(&uwsgi->greenmain, uwsgi->green_contexts[wsgi_req->async_id] );

	tstate = PyThreadState_GET();	
	tstate->recursion_depth = py_current_recursion_depth;
	tstate->frame = py_current_frame ;
}

PyObject *py_uwsgi_green_schedule(PyObject * self, PyObject * args) {

        struct wsgi_request *wsgi_req = current_wsgi_req(&uwsgi);

	/*
	int py_current_recursion_depth;
	struct _frame* py_current_frame;

	PyThreadState* tstate = PyThreadState_GET();	
	py_current_recursion_depth = tstate->recursion_depth;
	py_current_frame = tstate->frame;
	*/
	u_green_schedule_to_main(&uwsgi, wsgi_req->async_id);

	/*
	tstate = PyThreadState_GET();	
	tstate->recursion_depth = py_current_recursion_depth;
	tstate->frame = py_current_frame ;
	*/

	Py_INCREF(Py_True);
	return Py_True;

}

PyMethodDef uwsgi_green_methods[] = {
	{"green_schedule", py_uwsgi_green_schedule, METH_VARARGS, ""},
	{ NULL, NULL }
};

static struct wsgi_request *find_first_accepting_wsgi_req(struct uwsgi_server *uwsgi) {

        struct wsgi_request* wsgi_req = uwsgi->wsgi_requests ;
        int i ;

        for(i=0;i<uwsgi->async;i++) {
                if (wsgi_req->async_status == UWSGI_ACCEPTING) {
                        return wsgi_req ;
                }
                wsgi_req = next_wsgi_req(uwsgi, wsgi_req) ;
        }

        return NULL ;
}


static void u_green_request(struct uwsgi_server *uwsgi, struct wsgi_request *wsgi_req, int async_id) {


	for(;;) {
		wsgi_req_setup(wsgi_req, async_id);

		wsgi_req->async_status = UWSGI_ACCEPTING;

		u_green_schedule_to_main(uwsgi, async_id);

		if (wsgi_req_accept(uwsgi->serverfd, wsgi_req)) {
                        continue;
                }
		wsgi_req->async_status = UWSGI_OK;

		u_green_schedule_to_main(uwsgi, async_id);

                if (wsgi_req_recv(wsgi_req)) {
                        continue;
                }

		while(wsgi_req->async_status == UWSGI_AGAIN) {
			u_green_schedule_to_main(uwsgi, async_id);
			wsgi_req->async_status = (*uwsgi->shared->hooks[wsgi_req->modifier]) (uwsgi, wsgi_req);
		}

		u_green_schedule_to_main(uwsgi, async_id);

                uwsgi_close_request(uwsgi, wsgi_req);

	}

}

void u_green_loop(struct uwsgi_server *uwsgi) {

	struct wsgi_request *wsgi_req = uwsgi->wsgi_requests ;

	int i, current = 0 ;


	PyMethodDef *uwsgi_function;

	fprintf(stderr,"initializing %d green threads with stack size of %lu (%lu KB)\n", uwsgi->async, (unsigned long) GREEN_STACK_SIZE,  (unsigned long) GREEN_STACK_SIZE/1024);

	uwsgi->green_stacks = malloc( sizeof(char*) * uwsgi->async);
	if (!uwsgi->green_stacks) {
		perror("malloc()\n");
		exit(1);
	}

	for(i=0;i<uwsgi->async;i++) {
		//uwsgi->green_stacks[i] = malloc( 4096 * 256 );
		uwsgi->green_stacks[i] = mmap(NULL, GREEN_STACK_SIZE , PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS | MAP_PRIVATE | MAP_GROWSDOWN, -1, 0);
		if (!uwsgi->green_stacks[i]) {
			perror("mmap()");
			exit(1);
		}
	}


	uwsgi->green_contexts = malloc( sizeof(ucontext_t*) * uwsgi->async);
	if (!uwsgi->green_contexts) {
		perror("malloc()\n");
		exit(1);
	}


	for(i=0;i<uwsgi->async;i++) {
		uwsgi->green_contexts[i] = malloc( sizeof(ucontext_t) );
		if (!uwsgi->green_contexts[i]) {
			perror("malloc()");
			exit(1);
		}
		getcontext(uwsgi->green_contexts[i]);
		uwsgi->green_contexts[i]->uc_stack.ss_sp = uwsgi->green_stacks[i];
		uwsgi->green_contexts[i]->uc_stack.ss_size = GREEN_STACK_SIZE ;
		uwsgi->green_contexts[i]->uc_link = NULL;
		makecontext(uwsgi->green_contexts[i], (void (*) (void)) &u_green_request, 3, uwsgi, wsgi_req, i);
		wsgi_req->async_status = UWSGI_ACCEPTING;
		wsgi_req->async_id = i;
		wsgi_req = next_wsgi_req(uwsgi, wsgi_req) ;
	}

	for (uwsgi_function = uwsgi_green_methods; uwsgi_function->ml_name != NULL; uwsgi_function++) {
                PyObject *func = PyCFunction_New(uwsgi_function, NULL);
                PyDict_SetItemString(uwsgi->embedded_dict, uwsgi_function->ml_name, func);
                Py_DECREF(func);
        }


	for(;;) {

		uwsgi->async_running = green_blocking(uwsgi) ;

                uwsgi->async_nevents = async_wait(uwsgi->async_queue, uwsgi->async_events, uwsgi->async, uwsgi->async_running, 0);

                if (uwsgi->async_nevents < 0) {
                        continue;
                }

		if (i > 0) {
			wsgi_req = find_first_accepting_wsgi_req(uwsgi);
			if (!wsgi_req) goto cycle;
		}

                for(i=0; i<uwsgi->async_nevents;i++) {

                        if (uwsgi->async_events[i].ASYNC_FD == uwsgi->serverfd) {
				u_green_schedule_to_req(uwsgi, wsgi_req);
                        }

                }

cycle:
		wsgi_req = find_wsgi_req_by_id(uwsgi, current) ;
		if (wsgi_req->async_status != UWSGI_ACCEPTING) {
			u_green_schedule_to_req(uwsgi, wsgi_req);
		}
		current++;
		if (current >= uwsgi->async) current = 0;

	}

	// never here
	
}

#endif