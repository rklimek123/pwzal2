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
	q->back = -1;
	return q;
}

static void q_destroy(q_t** q) {
	if (q != NULL && *q != NULL) {
		if ((*q)->messages != NULL)
			free((*q)->messages);
		free(*q);
	}
}

static int q_size(q_t* q) {
	if (q == NULL || q->messages == NULL)
		return 0;
	else
		return q->cur_len;
}

static bool q_empty(q_t* q) {
	return q_size(q) <= 0;
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
		message_t* new_msgs = (message_t*)malloc(sizeof(message_t) * q->max_len / 2);
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
		q->max_len /= 2;
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
	pthread_mutex_t lock;
	void* state;
	bool active;

	int to_wait;
	int to_signal;
	pthread_mutex_t lock_thread;
	pthread_cond_t wait_for_msg[POOL_SIZE];
	int waiting;
} actor_t;

static size_t count_actors;
static actor_t* actors[CAST_LIMIT];

static size_t actors_finished;
static pthread_mutex_t state_counters_lock;


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
	
	if (pthread_mutex_init(&(a->lock), NULL) != 0) {
		q_destroy(&(a->msg_q));
		free(a);
		return NULL;
	}

	if (pthread_mutex_init(&(a->lock_thread), NULL) != 0) {
		pthread_mutex_destroy(&(a->lock));
		q_destroy(&(a->msg_q));
		free(a);
		return NULL;
	}

	for (int i = 0; i < POOL_SIZE; ++i) {
		if (pthread_cond_init(&(a->wait_for_msg[i]), NULL) != 0) {
			for (int j = 0; j < i; ++j) {
				pthread_cond_destroy(&(a->wait_for_msg[j]));
			}
			pthread_mutex_destroy(&(a->lock_thread));
			pthread_mutex_destroy(&(a->lock));
			q_destroy(&(a->msg_q));
			free(a);
			return NULL;
		}
	}
	

	a->state = NULL;
	a->active = false;
	a->to_wait = 0;
	a->to_signal = 0;
	a->waiting = 0;

	return a;
}

static void actor_destroy(actor_t** a) {
	for (int i = 0; i < POOL_SIZE; ++i) {
		pthread_cond_destroy(&((*a)->wait_for_msg[i]));
	}
	pthread_mutex_destroy(&((*a)->lock_thread));
	pthread_mutex_destroy(&((*a)->lock));
	q_destroy(&((*a)->msg_q));
	free(*a);
}

#define ACTOR_SUCCESS 0
#define ACTOR_DEAD -1
#define ACTOR_ERROR -2
#define ACTOR_IDLE -3

static int tp_notify(actor_id_t a);

static int actor_send_msg(actor_t* a, message_t msg) {
	if (pthread_mutex_lock(&(a->lock)) != 0)
		return ACTOR_ERROR;

	if (a->dead)
		return ACTOR_DEAD;

	int ret = q_push(a->msg_q, msg);

	if (ret != Q_SUCCESS) {
		return ACTOR_ERROR;
	}

	if (pthread_mutex_unlock(&(a->lock)) != 0)
		return ACTOR_ERROR;

	if (tp_notify(a->id) != 0)
		return ACTOR_ERROR;

	return ACTOR_SUCCESS;
}

// assumes you have a->lock acquired
static message_t actor_take_msg(actor_t* a) {
	if (q_empty(a->msg_q)) {
		exit(-1);
	}

	message_t ret = q_front(a->msg_q);

	if (q_pop(a->msg_q) != Q_SUCCESS)
		exit(-1);

	return ret;
}

static actor_t* actor_get(actor_id_t actor_id) {
	actor_t* a = NULL;

	if (pthread_mutex_lock(&state_counters_lock) != 0)
		return NULL;

	if ((size_t)actor_id < count_actors)
		a = actors[actor_id];

	if (pthread_mutex_unlock(&state_counters_lock) != 0)
		return NULL;

	return a;
}

static actor_t* actor_create(role_t* const role) {
	actor_t* a = actor_init();
	if (a == NULL)
		return NULL;

	if (pthread_mutex_lock(&state_counters_lock) != 0)
		return NULL;

	a->role = role;
	a->id = count_actors;
	actors[count_actors] = a;
	++count_actors;

	if (pthread_mutex_unlock(&state_counters_lock) != 0)
		return NULL;

	return a;
}

static message_t msg_hello(actor_t* a) {
	message_t new_msg;
	new_msg.message_type = MSG_HELLO;

	if (a == NULL) {
		new_msg.nbytes = 0;
		new_msg.data = NULL;
	}
	else {
		new_msg.nbytes = sizeof(actor_id_t*);
		new_msg.data = (void*)&(a->id);
	}
	
	return new_msg;
}

static int actor_handle_spawn(actor_t* a, message_t msg) {
	actor_t* new_a = actor_create((role_t*)msg.data);
	if (new_a == NULL)
		return ACTOR_ERROR;

	actor_send_msg(new_a, msg_hello(a));

	return ACTOR_SUCCESS;
}

static int actor_handle_godie(actor_t* a) {
	if (pthread_mutex_lock(&(a->lock)) != 0)
		return ACTOR_ERROR;
	
	if (!(a->dead)) {
		a->dead = true;
		
		if (pthread_mutex_lock(&state_counters_lock) != 0)
			return ACTOR_ERROR;

		++actors_finished;

		if (pthread_mutex_unlock(&state_counters_lock) != 0)
			return ACTOR_ERROR;
	}

	if (pthread_mutex_unlock(&(a->lock)) != 0)
		return ACTOR_ERROR;

	return ACTOR_SUCCESS;
}

static int actor_handle_message(actor_t* a, message_t msg) {
	message_type_t command = msg.message_type;

	if ((size_t)command >= a->role->nprompts)
		return ACTOR_ERROR;

	(a->role->prompts)[command](&(a->state), msg.nbytes, msg.data);
	return ACTOR_SUCCESS;
}

static pthread_key_t thread_number;

static int actor_exec(actor_t* a) {
	if (pthread_mutex_lock(&(a->lock)) != 0)
		return ACTOR_ERROR;

	if (q_empty(a->msg_q)) {
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


// tq - thread queue
typedef struct tq {
	int cur_len;
	int max_len;
	actor_id_t* actors;
	int front;
	int back;
} tq_t;

static tq_t* tq_init() {
	tq_t* q = (tq_t*)malloc(sizeof(tq_t));
	if (q == NULL)
		return NULL;
	
	q->cur_len = 0;
	q->max_len = 2;

	q->actors = (actor_id_t*)malloc(sizeof(actor_id_t) * 2);
	if (q->actors == NULL) {
		free(q);
		return NULL;
	}

	q->front = 0;
	q->back = -1;
	return q;
}

static void tq_destroy(tq_t** q) {
	if (*q != NULL) {
		if ((*q)->actors != NULL)
			free((*q)->actors);
		free(*q);
	}
}

static int tq_size(tq_t* q) {
	if (q == NULL || q->actors == NULL)
		return 0;
	else
		return q->cur_len;
}

static bool tq_empty(tq_t* q) {
	return tq_size(q) <= 0;
}

#define TQ_SUCCESS 0
#define TQ_EMPTY 1
#define TQ_BAD_ALLOC -1

static int tq_push(tq_t* q, actor_id_t a) {
	q->back = (q->back + 1) % q->max_len;
	q->actors[q->back] = a;
	++(q->cur_len);

	if (q->cur_len >= q->max_len) {
		actor_id_t* new_actors = (actor_id_t*)malloc(sizeof(actor_id_t) * q->max_len * 2);
		if (new_actors == NULL) {
			--(q->cur_len);
			q->back = (q->max_len + q->back - 1) % q->max_len;
			return TQ_BAD_ALLOC;
		}

		for (int i = 0; i < q->cur_len; ++i) {
			new_actors[i] = q->actors[(q->front + i) % q->max_len];
		}

		free(q->actors);
		q->actors = new_actors;
		q->front = 0;
		q->back = q->cur_len - 1;
		q->max_len *= 2;
	}

	return TQ_SUCCESS;
}

static int tq_pop(tq_t* q) {
	if (tq_empty(q))
		return TQ_EMPTY;

	q->front = (q->front + 1) % q->max_len;
	--(q->cur_len);

	if (q->cur_len < q->max_len / 4) {
		actor_id_t* new_actors = (actor_id_t*)malloc(sizeof(actor_id_t) * q->max_len / 2);
		if (new_actors == NULL) {
			++(q->cur_len);
			q->front = (q->max_len + q->front - 1) % q->max_len;
			return TQ_BAD_ALLOC;
		}

		for (int i = 0; i < q->cur_len; ++i) {
			new_actors[i] = q->actors[(q->front + i) % q->max_len];
		}

		free(q->actors);
		q->actors = new_actors;
		q->front = 0;
		q->back = q->cur_len - 1;
		q->max_len /= 2;
	}

	return TQ_SUCCESS;
}

static actor_id_t tq_front(tq_t* q) {
	return q->actors[q->front];
}

// tp - thread pool
typedef struct thread_pool {
	pthread_t* threads;
	actor_t** current_actor;
	void* keys;
	tq_t* thread_queue;
	pthread_mutex_t queue_mutex;
	pthread_cond_t wait_on_q;
} tp_t;

static tp_t* tp_init() {
	tp_t* tp = (tp_t*)malloc(sizeof(tp_t));
	tp->threads = (pthread_t*)malloc(sizeof(pthread_t) * POOL_SIZE);

	if (tp->threads == NULL)
		return NULL;

	tp->current_actor = (actor_t**)malloc(sizeof(actor_t*) * POOL_SIZE);

	if (tp->current_actor == NULL) {
		free(tp->threads);
		return NULL;
	}

	tp->keys = malloc(sizeof(int) * POOL_SIZE);

	if (tp->keys == NULL) {
		free(tp->current_actor);
		free(tp->threads);
		return NULL;
	}

	int* keys = (int*)tp->keys;

	for (int i = 0; i < POOL_SIZE; ++i) {
		keys[i] = i;
	}

	if (pthread_mutex_init(&(tp->queue_mutex), NULL) != 0) {
		free(tp->keys);
		free(tp->current_actor);
		free(tp->threads);
		return NULL;
	}

	if (pthread_cond_init(&(tp->wait_on_q), NULL) != 0) {
		pthread_mutex_destroy(&(tp->queue_mutex));
		free(tp->keys);
		free(tp->current_actor);
		free(tp->threads);
		return NULL;
	}

	if ((tp->thread_queue = tq_init()) == NULL) {
		pthread_cond_destroy(&(tp->wait_on_q));
		pthread_mutex_destroy(&(tp->queue_mutex));
		free(tp->keys);
		free(tp->current_actor);
		free(tp->threads);
		return NULL;
	}

	return tp;
}

static void tp_destroy(tp_t** tp) {
	tq_destroy(&((*tp)->thread_queue));
	pthread_cond_destroy(&((*tp)->wait_on_q));
	pthread_mutex_destroy(&((*tp)->queue_mutex));
	free((*tp)->current_actor);
	free((*tp)->keys);
	free((*tp)->threads);
	free(*tp);
}

// Module state
static _Atomic int running = 0;
static bool sigint_set = false;
static bool killed;
static tp_t* thread_pool;

static int tp_notify(actor_id_t a) {
	if (pthread_mutex_lock(&(thread_pool->queue_mutex)) != 0)
		return -1;

	int ret = tq_push(thread_pool->thread_queue, a);

	if (ret != TQ_SUCCESS) {
		return -1;
	}

	if (pthread_cond_signal(&(thread_pool->wait_on_q)) != 0)
		return -1;

	if (pthread_mutex_unlock(&(thread_pool->queue_mutex)) != 0)
		return -1;

	return 0;
}

static pthread_mutex_t join_mutex;
static pthread_cond_t waiting_to_endoperating;
static pthread_cond_t waiting_to_enddestroying;
static bool finished_operating;
static int joining;
static bool finished_destroying;
static int toexit;

static _Atomic int threads_finished;

static _Atomic int destroyed;
static bool registered_finished_operating;

static void reset_sigint() {
	struct sigaction sigint_action;
	sigint_action.sa_handler = SIG_DFL;
	sigint_action.sa_flags = SA_NODEFER;
	sigaction(SIGINT, &sigint_action, NULL);
}

static void module_destroy_state() {
	reset_sigint();
	pthread_cond_destroy(&waiting_to_endoperating);
	pthread_mutex_destroy(&state_counters_lock);
	pthread_key_delete(thread_number);
	tp_destroy(&thread_pool);

	for (int i = 0; i < CAST_LIMIT; ++i) {
		if (actors[i] != NULL) {
			actor_destroy(&(actors[i]));
		}
			
	}
}

static void tp_join() {
	if (pthread_mutex_lock(&join_mutex) != 0)
		exit(-1);
	++toexit;

	if (!registered_finished_operating) {
		++joining;
		while (!finished_operating) {
			if (pthread_cond_wait(&waiting_to_endoperating, &join_mutex) != 0)
				exit(-1);
		}
		registered_finished_operating = true;
		--joining;
		if (joining == 0) {
			if (pthread_mutex_unlock(&join_mutex) != 0)
				exit(-1);

			module_destroy_state();

			if (pthread_mutex_lock(&join_mutex) != 0)
				exit(-1);

			finished_destroying = true;
			if (pthread_cond_broadcast(&waiting_to_enddestroying) != 0)
				exit(-1);
		}
	}
	while (!finished_destroying) {
		if (pthread_cond_wait(&waiting_to_enddestroying, &join_mutex) != 0)
				exit(-1);
	}

	--toexit;

	if (pthread_mutex_unlock(&join_mutex) != 0)
		exit(-1);
	
	if (toexit == 0 && destroyed++ == 0) {
		pthread_cond_destroy(&waiting_to_enddestroying);
		pthread_mutex_destroy(&join_mutex);
		count_actors = 0;
		running = 0;
	}
}

static void stop_tp(int signo) {
	if (signo == SIGINT) {
		killed = true;
		pthread_cond_broadcast(&(thread_pool->wait_on_q));
		for (int i = 0; i < CAST_LIMIT; i++) {
			if (actors[i] != NULL) {
				for (int j = 0; j < POOL_SIZE; j++) {
					pthread_cond_broadcast(&(actors[i]->wait_for_msg[j]));
				}
			}
		}
	}
		
	else
		exit(-1);
	
	//tp_join();
}

static void set_sigint() {
	struct sigaction sigint_action;
	sigint_action.sa_handler = stop_tp;
	sigint_action.sa_flags = SA_RESTART | SA_RESETHAND;
	sigaction(SIGINT, &sigint_action, NULL);
	sigint_set = true;
}

static bool module_init_state() {
	if (running++ != 0)
		return false;
	
	killed = false;
	finished_operating = false;
	joining = 0;
	finished_destroying = false;
	toexit = 0;
	count_actors = 0;
	actors_finished = 0;
	threads_finished = 0;
	destroyed = 0;
	registered_finished_operating = false;

	if (!sigint_set) {
		set_sigint();
		sigint_set = true;
	}

	if ((thread_pool = tp_init()) == NULL)
		return false;

	if (pthread_key_create(&thread_number, NULL) != 0) {
		tp_destroy(&thread_pool);
		return false;
	}

	if (pthread_mutex_init(&state_counters_lock, NULL) != 0) {
		pthread_key_delete(thread_number);
		tp_destroy(&thread_pool);
		return false;
	}

	if (pthread_mutex_init(&join_mutex, NULL) != 0) {
		pthread_mutex_destroy(&state_counters_lock);
		pthread_key_delete(thread_number);
		tp_destroy(&thread_pool);
		return false;
	}

	if (pthread_cond_init(&waiting_to_endoperating, NULL) != 0) {
		pthread_mutex_destroy(&join_mutex);
		pthread_mutex_destroy(&state_counters_lock);
		pthread_key_delete(thread_number);
		tp_destroy(&thread_pool);
		return false;
	}

	if (pthread_cond_init(&waiting_to_enddestroying, NULL) != 0) {
		pthread_cond_destroy(&waiting_to_endoperating);
		pthread_mutex_destroy(&join_mutex);
		pthread_mutex_destroy(&state_counters_lock);
		pthread_key_delete(thread_number);
		tp_destroy(&thread_pool);
		return false;
	}

	return true;
}


// Threads
static void* thread_running(void* t_number) {
	if (pthread_setspecific(thread_number, t_number) != 0)
		exit(-1);

	size_t t_num = (size_t)(*((int*)t_number));

	while (actors_finished < count_actors) {
		if (killed)
			break;


		if (pthread_mutex_lock(&(thread_pool->queue_mutex)) != 0)
			exit(-1);

		while (tq_empty(thread_pool->thread_queue)) {
			if (killed) {
				if (pthread_mutex_unlock(&(thread_pool->queue_mutex)) != 0)
					exit(-1);
				break;
			}

			if (pthread_cond_wait(&(thread_pool->wait_on_q), &(thread_pool->queue_mutex)) != 0)
				exit(-1);
			
			if (killed) {
				if (pthread_mutex_unlock(&(thread_pool->queue_mutex)) != 0)
					exit(-1);
				break;
			}
		}

		if (killed) {
			if (pthread_mutex_unlock(&(thread_pool->queue_mutex)) != 0)
				exit(-1);
			break;
		}
		
		actor_id_t id_a = tq_front(thread_pool->thread_queue);
		tq_pop(thread_pool->thread_queue);
		actor_t* a = actor_get(id_a);

		if (pthread_mutex_unlock(&(thread_pool->queue_mutex)) != 0)
			exit(-1);


		if (pthread_mutex_lock(&(a->lock_thread)) != 0)
			exit(-1);

		while (a->active) {
			int to_wait = a->to_wait;
			a->to_wait = (a->to_wait + 1) % POOL_SIZE;
			a->waiting = a->waiting + 1;

			if (killed) {
				if (pthread_mutex_unlock(&(a->lock_thread)) != 0)
					exit(-1);
				break;
			}

			if (pthread_cond_wait(&(a->wait_for_msg[to_wait]), &(a->lock_thread)) != 0)
				exit(-1);
			
			if (killed) {
				if (pthread_mutex_unlock(&(a->lock_thread)) != 0)
					exit(-1);
				break;
			}
		}

		if (killed) {
			if (pthread_mutex_unlock(&(a->lock_thread)) != 0)
				exit(-1);
			break;
		}
		
		a->active = true;
		
		if (pthread_mutex_unlock(&(a->lock_thread)) != 0)
			exit(-1);


		thread_pool->current_actor[t_num] = a;

		int check = actor_exec(a);

		if (check == ACTOR_ERROR)
			exit(-1);


		if (pthread_mutex_lock(&(a->lock_thread)) != 0)
			exit(-1);
		
		a->active = false;

		if (a->waiting > 0) {
			int to_signal = a->to_signal;
			a->to_signal = (a->to_signal + 1) % POOL_SIZE;
			a->waiting = a->waiting - 1;

			if (pthread_cond_signal(&(a->wait_for_msg[to_signal])) != 0)
				exit(-1);
		}

		if (pthread_mutex_unlock(&(a->lock_thread)) != 0)
			exit(-1);
	}

	killed = true;


	if (pthread_mutex_lock(&(thread_pool->queue_mutex)) != 0)
		exit(-1);

	if (pthread_cond_broadcast(&(thread_pool->wait_on_q)) != 0)
		exit(-1);

	if (pthread_mutex_unlock(&(thread_pool->queue_mutex)) != 0)
		exit(-1);


	if (++threads_finished >= POOL_SIZE) {
		int ret;

		for (int i = 0; i < POOL_SIZE; ++i) {
			if ((size_t)i != t_num && pthread_join(thread_pool->threads[i], (void **)(&ret)) == 35)
				exit(-1);
		}

		if (pthread_mutex_lock(&join_mutex) != 0)
			exit(-1);
		
		finished_operating = true;
		if (pthread_cond_broadcast(&waiting_to_endoperating) != 0)
			exit(-1);

		if (pthread_mutex_unlock(&join_mutex) != 0)
			exit(-1);
	}

	return 0;
}


// Interface
void actor_system_join(actor_id_t actor) {
	actor_t* a = actor_get(actor);
	if (a == NULL)
		exit(-1);

	tp_join();
}

int actor_system_create(actor_id_t* actor, role_t* const role) {
	if (!module_init_state())
		return -1;

	actor_t* a = actor_create(role);

	if (a == NULL) {
		module_destroy_state();
		return -1;
	}
	
	for (int i = 0; i < POOL_SIZE; ++i) {
		if (pthread_create(&(thread_pool->threads[i]),
						NULL,
						thread_running,
						(void*)&(((int*)(thread_pool->keys))[i])) != 0) {
			exit(-1);
		}
	}

	*actor = a->id;

	send_message(a->id, msg_hello(NULL));

	return 0;
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
	int* t_num_ptr = (int*)pthread_getspecific(thread_number);

	if (t_num_ptr == NULL)
		exit(-1);

	actor_t* a = thread_pool->current_actor[*t_num_ptr];
	return a->id;
}
