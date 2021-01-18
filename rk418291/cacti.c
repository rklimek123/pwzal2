#include "cacti.h"

// q - queue
typedef struct queue {
	int cur_len;
	int max_len;
	int limit;
	message_t* messages;
	int front;
	int back;
} q_t;

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
	return q;
}

static void q_destroy(q_t** q) {
	if (*q != NULL) {
		free((*q)->messages);
		free(*q);
	}
}

static int q_size(q_t* q) {
	if (q == NULL)
		return 0;
	else
		return q->cur_len;
}

static bool q_empty(q_t* q) {
	return q_size(q) == 0;
}

static bool q_full(q_t* q) {
	return q_size(q) == q->limit;
}

#define Q_SUCCESS 0
#define Q_FULL 1
#define Q_EMPTY 2
#define Q_BAD_ALLOC -1

static int q_push(q_t* q, message_t msg) {
	if (q_full(q))
		return Q_FULL;

	q->back = (q->back + 1) % q->max_len;
	q->messages[q->back] = msg;
	++(q->cur_len);

	if (q->cur_len >= q->max_len && q->max_len != q->limit) {
		message_t* new_msgs = (message_t*)malloc(sizeof(message_t) * q->max_len * 2);
		if (new_msgs == NULL) {
			--(q->cur_len);
			q->back = (q->max_len + q->back - 1) % q->max_len;
			return Q_BAD_ALLOC;
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

	return Q_SUCCESS;
}

static int q_pop(q_t* q) {
	if (q_empty(q))
		return Q_EMPTY;

	q->front = (q->front + 1) % q->max_len;
	--(q->cur_len);

	if (q->cur_len < q->max_len / 4) {
		message_t* new_msgs = (message_t*)malloc(sizeof(message_t) * q->max_len / 4);
		if (new_msgs == NULL) {
			++(q->cur_len);
			q->front = (q->max_len + q->front - 1) % q->max_len;
			return Q_BAD_ALLOC;
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

	return Q_SUCCESS;
}

static message_t q_front(q_t* q) {
	return q->messages[q->front];
}


// actor
typedef struct actor {
	q_t* msg_q;
	role_t const* role;
	actor_id_t id;
	bool dead;
	bool finished;
	pthread_mutex_t lock;
	pthread_cond_t wait_for_msg;
} actor_t;

static size_t count_actors = 0;
static actor_t* actors[CAST_LIMIT];

// counts actors whose queues are empty.
static size_t actors_finished = 0;
static pthread_mutex_t* state_counters_lock = NULL;


static actor_t* actor_init() {
	actor_t* a = (actor_t*)malloc(sizeof(actor_t));
	if (a == NULL)
		return NULL;
	
	a->msg_q = q_init();
	if (a->msg_q == NULL) {
		free(a);
		return NULL;
	}
	
	a->dead = false;
	a->finished = false;
	
	if (pthread_mutex_init(&(a->lock), NULL) != 0) {
		q_destroy(&(a->msg_q));
		free(a);
		return NULL;
	}

	if (pthread_cond_init(&(a->wait_for_msg), NULL) != 0) {
		pthread_mutex_destroy(&(a->lock));
		q_destroy(&(a->msg_q));
		free(a);
		return NULL;
	}

	return a;
}

static void actor_destroy(actor_t** a) {
	pthread_cond_destroy(&((*a)->wait_for_msg));
	pthread_mutex_destroy(&((*a)->lock));
	q_destroy(&((*a)->msg_q));
	free(a);
}

#define ACTOR_SUCCESS 0
#define ACTOR_DEAD -1
#define ACTOR_ERROR -2
#define ACTOR_IDLE -3

static int actor_send_msg(actor_t* a, message_t msg) {
	if (pthread_mutex_lock(&(a->lock)) != 0)
		return ACTOR_ERROR;

	if (a->dead)
		return ACTOR_DEAD;

	int ret = q_push(a->msg_q, msg);

	if (ret != Q_SUCCESS) {
		return ACTOR_ERROR;
	}

	if (a->finished) {
		if (pthread_mutex_lock(state_counters_lock) != 0)
			return ACTOR_ERROR;

		a->finished = false;
		--actors_finished;

		if (pthread_mutex_unlock(state_counters_lock) != 0)
			return ACTOR_ERROR;
	}

	if (pthread_cond_signal(&(a->wait_for_msg)) != 0)
		return ACTOR_ERROR;

	if (pthread_mutex_unlock(&(a->lock)) != 0)
		return ACTOR_ERROR;

	return ACTOR_SUCCESS;
}

// assumes you have a->lock acquired
static message_t actor_take_msg(actor_t* a) {
	while (q_empty(a->msg_q)) { // trzeba za³atwiæ nieblokuj¹co, (bonus: ale ¿eby nie by³o aktywnego oczekiwania dla threadu tez)
		if (pthread_cond_wait(&(a->wait_for_msg), &(a->lock)) != 0)
			exit(-1);
	}

	message_t ret = q_front(a->msg_q);

	if (q_pop(a->msg_q) != Q_SUCCESS)
		exit(-1);

	if (q_empty(a->msg_q)) {
		if (pthread_mutex_lock(state_counters_lock) != 0)
			exit(-1);

		a->finished = true;
		++actors_finished;

		if (pthread_mutex_unlock(state_counters_lock) != 0)
			exit(-1);
	}

	if (pthread_cond_signal(&(a->wait_for_msg)) != 0)
		exit(-1);

	return ret;
}

static actor_t* actor_get(actor_id_t actor_id) {
	actor_t* a = NULL;

	if (pthread_mutex_lock(state_counters_lock) != 0)
		return NULL;

	if ((size_t)actor_id < count_actors)
		a = actors[actor_id];

	if (pthread_mutex_unlock(state_counters_lock) != 0)
		return NULL;

	return a;
}

static actor_t* actor_create(role_t* const role) {
	actor_t* a = actor_init();
	if (a == NULL)
		return NULL;

	if (pthread_mutex_lock(state_counters_lock) != 0)
		return NULL;

	a->role = role;
	a->id = count_actors;
	actors[count_actors] = a;
	++count_actors;

	if (pthread_mutex_unlock(state_counters_lock) != 0)
		return NULL;

	return a;
}

static int actor_handle_spawn(actor_t* a, message_t msg) {
	actor_t* new_a = actor_create((role_t*)msg.data);
	if (new_a == NULL)
		return ACTOR_ERROR;

	message_t new_msg;
	new_msg.message_type = MSG_HELLO;
	new_msg.nbytes = sizeof(actor_id_t*);
	new_msg.data = (void*)&(a->id);
	actor_send_msg(new_a, new_msg);

	return ACTOR_SUCCESS;
}

static int actor_handle_godie(actor_t* a) {
	if (pthread_mutex_lock(&(a->lock)) != 0)
		return ACTOR_ERROR;
	
	q_destroy(&(a->msg_q));
	a->dead = true;

	if (pthread_mutex_unlock(&(a->lock)) != 0)
		return ACTOR_ERROR;

	return ACTOR_SUCCESS;
}

static int actor_handle_message(actor_t* a, message_t msg) {
	message_type_t command = msg.message_type;

	if ((size_t)command >= a->role->nprompts)
		return ACTOR_ERROR;

	(a->role->prompts)[command](NULL, msg.nbytes, msg.data);
	return ACTOR_SUCCESS;
}

static int actor_exec(actor_t* a) {
	if (pthread_mutex_lock(&(a->lock)) != 0)
		return ACTOR_ERROR;

	if (q_empty(a->msg_q)) { // rozwi¹zanie nieblokuj¹ce, ale z aktywnym oczekiwaniem
		if (pthread_mutex_unlock(&(a->lock)) != 0)
			return ACTOR_ERROR;
		return ACTOR_IDLE;
	}
		
	message_t msg = actor_take_msg(a);

	if (pthread_mutex_unlock(&(a->lock)) != 0)
		return ACTOR_ERROR;

	switch (msg.message_type) {
		case MSG_SPAWN:
			return actor_handle_spawn(a, msg);

		case MSG_GODIE:
			return actor_handle_godie(a);

		default:
			return actor_handle_message(a, msg);
	}
}


// tp - thread pool
typedef struct thread_pool {
	pthread_t** threads;
	actor_t** current_actor;
} tp_t;

static tp_t* tp_init() {
	tp_t* tp = (tp_t*)malloc(sizeof(tp_t));
	tp->threads = (pthread_t**)malloc(sizeof(pthread_t*) * POOL_SIZE);
	tp->current_actor = (actor_t**)malloc(sizeof(actor_t*) * POOL_SIZE);

	if (tp->threads == NULL)
		return NULL;

	return tp;
}

static void tp_join(tp_t* tp) {
	int retval;

	for (int i = 0; i < POOL_SIZE; ++i) {
		if (tp->threads[i] != NULL) {
			pthread_join(*(tp->threads[i]), (void**)&retval);
		}
	}
}

static void tp_destroy(tp_t** tp) {
	tp_join(*tp);
	free((*tp)->threads);
	free((*tp)->current_actor);
	free(*tp);
}


// Module state
static _Atomic int running = 0;
static bool killed = false;
static bool sigint_set = false;
static tp_t* thread_pool = NULL;
static pthread_key_t* thread_number = NULL;

static int threads_finished = 0;
static pthread_mutex_t* thread_finish_lock = NULL;

static void change_flag(int signo) {
	if (signo == SIGINT)
		killed = true;
	else
		exit(-1);
}

static void set_sigint() {
	struct sigaction sigint_action;
	sigint_action.sa_handler = change_flag;
	sigint_action.sa_flags = SA_RESTART;
	sigaction(SIGINT, &sigint_action, NULL);
}

static bool module_init_state() {
	if (running++ != 0)
		return false;

	if (!sigint_set)
		set_sigint();

	for (int i = 0; i < CAST_LIMIT; ++i) {
		if (actors[i] != NULL)
			actor_destroy(&(actors[i]));
	}

	if ((thread_pool = tp_init()) == NULL)
		return false;

	for (int i = 0; i < POOL_SIZE; ++i) {
		thread_pool->threads[i] = NULL;
	}

	if (pthread_key_create(thread_number, NULL) != 0) {
		tp_destroy(&thread_pool);
		return false;
	}

	if (pthread_mutex_init(state_counters_lock, NULL) != 0) {
		pthread_key_delete(*thread_number);
		tp_destroy(&thread_pool);
		return false;
	}

	count_actors = 0;
	actors_finished = 0;

	if (pthread_mutex_init(thread_finish_lock, NULL) != 0) {
		pthread_mutex_destroy(state_counters_lock);
		pthread_key_delete(*thread_number);
		tp_destroy(&thread_pool);
		return false;
	}

	threads_finished = 0;
	return true;
}

static void module_destroy_state() {
	pthread_mutex_destroy(thread_finish_lock);
	pthread_mutex_destroy(state_counters_lock);
	pthread_key_delete(*thread_number);
	tp_destroy(&thread_pool);

	for (int i = 0; i < CAST_LIMIT; ++i) {
		if (actors[i] != NULL)
			actor_destroy(&(actors[i]));
	}

	count_actors = 0;
	threads_finished = 0;
	running = 0;
}


// Threads
static void* thread_running(void* t_number) {
	if (pthread_setspecific(*thread_number, t_number) != 0)
		exit(-1);

	size_t t_num = *((size_t *)t_number);
	size_t i = t_num;

	while (actors_finished < count_actors) {
		if (killed)
			break;

		actor_t* a = actors[i];
		thread_pool->current_actor[t_num] = a;

		if (a != NULL) {
			int check = actor_exec(a);
			if (check == ACTOR_ERROR)
				exit(-1);
		}

		i += POOL_SIZE;
		if (i > count_actors)
			i = t_num;
	}

	if (pthread_mutex_lock(thread_finish_lock) != 0)
		exit(-1);

	int tf = ++threads_finished;

	if (pthread_mutex_unlock(thread_finish_lock) != 0)
		exit(-1);

	if (tf == POOL_SIZE)
		module_destroy_state();

	return 0;
}


// Interface
int actor_system_create(actor_id_t* actor, role_t* const role) {
	if (!module_init_state())
		return -1;

	actor_t* a = actor_create(role);

	if (a == NULL) {
		module_destroy_state();
		return -1;
	}

	message_t first_msg;
	first_msg.message_type = MSG_HELLO;
	first_msg.nbytes = 0;
	first_msg.data = NULL;

	if (actor_send_msg(a, first_msg) != ACTOR_SUCCESS) {
		actor_destroy(&a);
		module_destroy_state();
		return -1;
	}
	
	for (int i = 0; i < POOL_SIZE; ++i) {
		if (pthread_create(thread_pool->threads[i], NULL, thread_running, (void*)&i) != 0) {
			exit(-1);
		}
	}

	*actor = a->id;
	return 0;
}

void actor_system_join(actor_id_t actor) {
	actor_t* a = actor_get(actor);
	if (a == NULL)
		exit(-1); 

	tp_join(thread_pool);
	module_destroy_state();
}

#define SM_SUCCESS 0
#define SM_ACTOR_DEAD -1
#define SM_ACTOR_NEXISTS -2
#define SM_ERROR -3

int send_message(actor_id_t actor, message_t message) {
	actor_t* a = actor_get(actor);

	if (a == NULL)
		return SM_ACTOR_NEXISTS;

	int ret = actor_send_msg(a, message);
	
	switch (ret) {
		case ACTOR_SUCCESS:
			return SM_SUCCESS;

		case ACTOR_DEAD:
			return SM_ACTOR_DEAD;

		case ACTOR_ERROR:
			return SM_ERROR;

		default:
			exit(-1);
	}
}

actor_id_t actor_id_self() {
	size_t* t_num_ptr = (size_t*)pthread_getspecific(*thread_number);

	if (t_num_ptr == NULL)
		exit(-1);

	actor_t* a = thread_pool->current_actor[*t_num_ptr];
	return a->id;
}
