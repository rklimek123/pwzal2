#include "cacti.h"

// tp - thread pool
static struct thread_pool {
	pthread_t** threads;
};
typedef struct thread_pool tp_t;

static tp_t* tp_init() {
	tp_t* tp = (tp_t*)malloc(sizeof(tp_t));
	tp->threads = (pthread_t**)malloc(sizeof(pthread_t*) * POOL_SIZE);
	
	if (tp->threads == NULL)
		return NULL;

	for (int i = 0; i < POOL_SIZE; ++i) {
		tp->threads[i] = (pthread_t*)malloc(sizeof(pthread_t));

		if (tp->threads[i] == NULL) {
			for (int j = 0; j < i; ++j) {
				free(tp->threads[j]);
			}
			
			free(tp->threads);
			return NULL;
		}
	}
	
	return tp;
}

static void tp_destroy(tp_t** tp) {
	// koñczenie w¹tków, czy potrzebne?
	// np. tym joinem?

	for (int i = 0; i < POOL_SIZE; ++i) {
		pthread_join(*((*tp)->threads[i]), NULL);
		free((*tp)->threads[i]);
	}

	free((*tp)->threads);
	free(*tp);
}


// q - queue
static struct queue {
	int cur_len;
	int max_len;
	int limit;
	message_t* messages;
	int front;
	int back;
};
typedef struct queue q_t;

static q_t* q_init() {
	q_t* q = (q_t*)malloc(sizeof(q_t));
	if (q == NULL)
		return NULL;
	
	q->cur_len = 0;
	q->max_len = 2;
	q->limit = ACTOR_QUEUE_LIMIT;

	q->messages = (message_t*)malloc(sizeof(message_t) * 2);
	if (q->messages == NULL) {
		free(q);
		return NULL;
	}

	q->front = 0;
	q->back = 0;
}

static void q_destroy(q_t** q) {
	free(q->messages);
	free(q);
}

static bool q_empty(q_t* q) {
	return q->cur_len == 0;
}

static bool q_full(q_t* q) {
	return q->cur_len == q->limit;
}

static bool q_put(q_t* q, message_t msg) {
	if (q_full(q))
		return false;
	q->back = (q->back + 1) % q->max_len;
	q->messages[q->back] = msg;
	++(q->cur_len);

	if (q->cur_len >= q->max_len && q->max_len != q->limit) {
		message_t* new_msgs = (message_t*)malloc(sizeof(message_t) * q->max_len * 2);
		if (new_msgs == NULL) {
			--(q->cur_len);
			q->back = (q->max_len + q->back - 1) % q->max_len;
			return false;
		}

		for (int i = 0; i < q->cur_len; ++i) {
			new_msgs[i] = q->messages[(q->front + i) % q->max_len];
		}

		free(q->messages);
		q->messages = new_msgs;
		q->front = 0;
		q->back = q->cur_len - 1;
		q->max_len *= 2;
	}

	return true;
}

static bool q_pop(q_t* q) {
	if (q_empty(q))
		return false;

	q->front = (q->front + 1) % q->max_len;
	--(q->cur_len);

	if (q->cur_len < q->max_len / 4) {
		message_t* new_msgs = (message_t*)malloc(sizeof(message_t) * q->max_len / 4);
		if (new_msgs == NULL) {
			++(q->cur_len);
			q->front = (q->max_len + q->front - 1) % q->max_len;
			return false;
		}

		for (int i = 0; i < q->cur_len; ++i) {
			new_msgs[i] = q->messages[(q->front + i) % q->max_len];
		}

		free(q->messages);
		q->messages = new_msgs;
		q->front = 0;
		q->back = q->cur_len - 1;
		q->max_len /= 4;
	}

	return true;
}

static message_t q_front(q_t* q) {
	return q->messages[q->front];
}


// actor
static struct actor {
	q_t* msg_q;
	bool dead;
	static pthread_mutex_t* q_lock;
};
typedef struct actor actor_t;

static actor_t* actor_init() {
	actor_t a* = (actor_t*)malloc(sizeof(actor_t));
	if (a == NULL)
		return NULL;
	
	a->msg_q = q_init();
	if (a->msg_q == NULL) {
		free(a);
		return NULL;
	}
	
	dead = false;
	
	if (pthread_mutex_init(a->q_mutex, 0) != 0) {
		q_destroy(&(a->msg_q));
		free(a);
		return NULL;
	}

	return a;
}

static void actor_destroy(actor_t** a) {
	pthread_mutex_destroy((*a)->q_lock);
	q_destroy(&((*a)->msg_q));
	free(a);
}


// Global variables
static bool running = false;
static size_t count_actors = 0;
static tp_t* thread_pool = NULL;
static pthread_mutex_t* check_actor_mutex = NULL;

static bool module_init() {
	if ((thread_pool = tp_init()) == NULL)
		return false

	if (pthread_mutex_init(check_actor_mutex, 0) != 0) {
		tp_destroy(&thread_pool);
		return false;
	}

	running = true;
	return true;
}


// Interface
int actor_system_create(actor_id_t* actor, role_t* const role) {
	if (count_actors > 0)
		return -1;

	++count_actors;
	//zrob actora;
	//daj mu funkcjê na rozpoczecie i zakonczenie przetwarzania;
	// funkcja ta parametryzowana przez role;
	// actor_t* actr; malloc
	// actor = actr
	// jak cos sie wypieprzy³o to daj -1
	return 0;
}

void actor_system_join(actor_id_t actor) {
	// argument actor_id_t actor? czy jest potrzebny do czegos??
}

int send_message(actor_id_t actor, message_t message) {
	// lock!!!!
}

actor_id_t actor_id_self();
