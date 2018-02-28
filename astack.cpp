/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <atomic>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <jvmti.h>

struct AsyncCallFrame {
   jint lineno;
   jmethodID method;
};

struct AsyncCallTrace {
   JNIEnv *jni;
   jint num_frames;
   AsyncCallFrame *frames;
};

extern "C"
void AsyncGetCallTrace(AsyncCallTrace *trace, jint depth, void *ucontext)
__attribute__ ((weak));

struct ThreadTag {
   JNIEnv *jni;
   pthread_t thread_id;
};

static const jint NATIVE_METHOD_LINENO = -3; // value used by JVM
static const int SIGSTACK = SIGPWR; // arbitrary unused signal
static const int MAX_FRAMES = 128;

static int port;

static AsyncCallTrace x_trace;
static AsyncCallFrame x_frames[MAX_FRAMES];
static std::atomic_flag x_trace_running;
static jrawMonitorID x_trace_lock;

static bool ok(jvmtiError err)
{
   return err == JVMTI_ERROR_NONE;
}

static void fixClassSignature(char *s)
{
   size_t len = strlen(s);
   if ((len <= 2) || (s[0] != 'L') || (s[len - 1] != ';')) {
      return;
   }

   memmove(s, s + 1, len - 2);
   len -= 2;
   s[len] = '\0';

   for (size_t i = 0; i < len; i++) {
      if (s[i] == '/') {
         s[i] = '.';
      }
   }
}

static jint getLineNumber(jvmtiEnv *jvmti, jmethodID method, jlocation target)
{
   if (target < 0) {
      return target;
   }

   jint count;
   jvmtiLineNumberEntry *table;
   if (!ok(jvmti->GetLineNumberTable(method, &count, &table))) {
      return -1;
   }

   jint line_number = -1;
   if (count == 1) {
      line_number = table[0].line_number;
   }
   else if (count > 1) {
      jlocation last = table[0].start_location;
      for (int i = 1; i < count; i++) {
         if ((target < table[i].start_location) && (target >= last)) {
            line_number = table[i - 1].line_number;
            break;
         }
         last = table[i].start_location;
      }
      if ((line_number == -1) && (target >= last)) {
         line_number = table[count - 1].line_number;
      }
   }

   jvmti->Deallocate((unsigned char *) table);
   return line_number;
}

static const char *threadStateEnum(jint state)
{
   if (state & JVMTI_THREAD_STATE_ALIVE) {
      if (state & JVMTI_THREAD_STATE_RUNNABLE) {
         return "RUNNABLE";
      }
      if (state & JVMTI_THREAD_STATE_BLOCKED_ON_MONITOR_ENTER) {
         return "BLOCKED (on object monitor)";
      }
      if (state & JVMTI_THREAD_STATE_WAITING_INDEFINITELY) {
         if (state & JVMTI_THREAD_STATE_IN_OBJECT_WAIT) {
            return "WAITING (on object monitor)";
         }
         if (state & JVMTI_THREAD_STATE_PARKED) {
            return "WAITING (parking)";
         }
         return "WAITING";
      }
      if (state & JVMTI_THREAD_STATE_WAITING_WITH_TIMEOUT) {
         if (state & JVMTI_THREAD_STATE_IN_OBJECT_WAIT) {
            return "TIMED_WAITING (on object monitor)";
         }
         if (state & JVMTI_THREAD_STATE_PARKED) {
            return "TIMED_WAITING (parking)";
         }
         if (state & JVMTI_THREAD_STATE_SLEEPING) {
            return "TIMED_WAITING (sleeping)";
         }
         return "TIMED_WAITING";
      }
      return "UNKNOWN";
   }
   if (state & JVMTI_THREAD_STATE_TERMINATED) {
      return "TERMINATED";
   }
   return "NEW";
}

static void printCallFrame(jvmtiEnv *jvmti, JNIEnv *jni, jmethodID method, jint lineno, FILE *out)
{
   char *method_name;
   if (!ok(jvmti->GetMethodName(method, &method_name, nullptr, nullptr))) {
      method_name = nullptr;
   }

   jclass clazz;
   char *class_name = nullptr;
   char *source_name = nullptr;
   if (ok(jvmti->GetMethodDeclaringClass(method, &clazz))) {
      if (ok(jvmti->GetClassSignature(clazz, &class_name, nullptr))) {
         fixClassSignature(class_name);
      }
      else {
         class_name = nullptr;
      }
      if (!ok(jvmti->GetSourceFileName(clazz, &source_name))) {
         source_name = nullptr;
      }
      jni->DeleteLocalRef(clazz);
   }

   jint line_number = getLineNumber(jvmti, method, lineno);

   const char *class_text = class_name ?: "Unknown";
   const char *method_text = method_name ?: "Unknown";

   if (line_number == NATIVE_METHOD_LINENO) {
      fprintf(out, "\tat %s.%s(Native Method)\n", class_text, method_text);
   }
   else if (source_name == nullptr) {
      fprintf(out, "\tat %s.%s(Unknown Source)\n", class_text, method_text);
   }
   else if (line_number <= 0) {
      fprintf(out, "\tat %s.%s(%s)\n", class_text, method_text, source_name);
   }
   else {
      fprintf(out, "\tat %s.%s(%s:%d)\n", class_text, method_text, source_name, line_number);
   }

   jvmti->Deallocate((unsigned char *) method_name);
   jvmti->Deallocate((unsigned char *) class_name);
   jvmti->Deallocate((unsigned char *) source_name);
}

static void printThreadDump(jvmtiEnv *jvmti, JNIEnv *jni, jthread thread, FILE *out)
{
   jint state;
   jvmtiThreadInfo info;
   if (ok(jvmti->GetThreadState(thread, &state)) &&
         ok(jvmti->GetThreadInfo(thread, &info))) {
      fprintf(out,
         "\"%s\"%s prio=%d\n"
         "  java.lang.Thread.Stage: %s\n",
         info.name,
         info.is_daemon ? " daemon" : "",
         info.priority,
         threadStateEnum(state));

      jvmti->Deallocate((unsigned char *) info.name);
      jni->DeleteLocalRef(info.thread_group);
      jni->DeleteLocalRef(info.context_class_loader);
   }

   for (int i = 0; i < x_trace.num_frames; i++) {
      AsyncCallFrame *frame = &(x_trace.frames[i]);
      printCallFrame(jvmti, jni, frame->method, frame->lineno, out);
   }
   fprintf(out, "\n");
}

static void dumpThread(jvmtiEnv *jvmti, JNIEnv *jni, jthread thread, FILE *out)
{
   jvmti->RawMonitorEnter(x_trace_lock);

   ThreadTag *tag;
   if (!ok(jvmti->GetTag(thread, (jlong *) &tag)) || (tag == nullptr)) {
      jvmti->RawMonitorExit(x_trace_lock);
      return;
   }

   x_trace.frames = x_frames;
   x_trace.jni = tag->jni;
   x_trace_running.test_and_set();

   pthread_kill(tag->thread_id, SIGSTACK);

   // spin until trace is finished
   bool done = false;
   for (int i = 0; i < (100 * 1000 * 1000); i++) {
      if (!x_trace_running.test_and_set()) {
         done = true;
         break;
      }
   }

   jvmti->RawMonitorExit(x_trace_lock);

   if (done) {
      printThreadDump(jvmti, jni, thread, out);
   }
   else {
      fprintf(stderr, "WARNING: AStack trace did not complete\n");
   }
}

static void handleClient(jvmtiEnv *jvmti, JNIEnv *jni, FILE *out)
{
   jint count;
   jthread *threads;
   auto err = jvmti->GetAllThreads(&count, &threads);
   if (!ok(err)) {
      fprintf(stderr, "WARNING: GetAllThreads failed: %d\n", err);
      return;
   }

   for (int i = 0; i < count; i++) {
      auto thread = threads[i];
      dumpThread(jvmti, jni, thread, out);
      jni->DeleteLocalRef(thread);
   }

   jvmti->Deallocate((unsigned char *) threads);
}


static int serverSocket()
{
   int fd = socket(AF_INET6, SOCK_STREAM, 0);
   if (fd == -1) {
      perror("ERROR: failed to create AStack socket");
      exit(1);
   }

   int reuse = true;
   if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
      perror("ERROR: failed to set AStack socket option");
      exit(1);
   }

   sockaddr_in6 addr = {};
   addr.sin6_family = AF_INET6;
   addr.sin6_port = htons(port);
   addr.sin6_addr = in6addr_any;

   if (bind(fd, (sockaddr *) &addr, sizeof(addr)) == -1) {
      perror("ERROR: failed to bind AStack socket");
      exit(1);
   }

   if (listen(fd, SOMAXCONN) == -1) {
      perror("ERROR: failed to listen on AStack socket");
      exit(1);
   }

   return fd;
}

static void JNICALL worker(jvmtiEnv *jvmti, JNIEnv *jni, void *arg)
{
   int server = serverSocket();

   fprintf(stderr, "AStack listener started on port %d\n", port);

   while (true) {
      int client = accept(server, nullptr, 0);
      if (client != -1) {
         FILE *out = fdopen(client, "w");
         handleClient(jvmti, jni, out);
         fclose(out);
      }
   }
}

static jthread createThread(JNIEnv *jni, const char *threadName)
{
   auto clazz = jni->FindClass("java/lang/Thread");
   if (clazz == nullptr) {
      fprintf(stderr, "ERROR: AStack: failed to find Thread class\n");
      exit(1);
   }

   auto init = jni->GetMethodID(clazz, "<init>", "(Ljava/lang/String;)V");
   if (init == nullptr) {
      fprintf(stderr, "ERROR: AStack: failed to find Thread constructor\n");
      exit(1);
   }

   auto name = jni->NewStringUTF(threadName);
   if (name == nullptr) {
      fprintf(stderr, "ERROR: AStack: failed to create String object\n");
      exit(1);
   }

   auto object = jni->NewObject(clazz, init, name);
   if (object == nullptr) {
      fprintf(stderr, "ERROR: AStack: failed to create Thread object\n");
      exit(1);
   }

   return object;
}

static void createMethodIDs(jvmtiEnv *jvmti, jclass clazz)
{
   // allocate method IDs for AsyncGetCallTrace
   jint count;
   jmethodID *methods;
   auto err = jvmti->GetClassMethods(clazz, &count, &methods);
   if (err == JVMTI_ERROR_NONE) {
      jvmti->Deallocate((unsigned char *) methods);
   }
   else if (err != JVMTI_ERROR_CLASS_NOT_PREPARED) {
      fprintf(stderr, "WARNING: GetClassMethods failed: %d\n", err);
   }
}

static void signalHandler(int sig, siginfo_t *info, void *ucontext)
{
   AsyncGetCallTrace(&x_trace, MAX_FRAMES, ucontext);
   x_trace_running.clear();
}

static void JNICALL onVmInit(jvmtiEnv *jvmti, JNIEnv *jni, jthread thread)
{
   jvmtiError err;

   // allocate method IDs for already loaded classes
   jint count;
   jclass *classes;
   err = jvmti->GetLoadedClasses(&count, &classes);
   if (!ok(err)) {
      fprintf(stderr, "ERROR: GetLoadedClasses failed: %d\n", err);
      exit(1);
   }
   for (int i = 0; i < count; i++) {
      createMethodIDs(jvmti, classes[i]);
   }
   jvmti->Deallocate((unsigned char *) classes);

   // install signal handler needed to invoke AsyncGetCallTrace
   struct sigaction sa;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = SA_SIGINFO;
   sa.sa_sigaction = signalHandler;
   if (sigaction(SIGSTACK, &sa, nullptr) == -1) {
      perror("ERROR: failed to install AStack signal handler");
      exit(1);
   }

   // start agent worker thread
   auto agent = createThread(jni, "AStack Listener");
   err = jvmti->RunAgentThread(agent, &worker, nullptr, JVMTI_THREAD_MAX_PRIORITY);
   if (!ok(err)) {
      fprintf(stderr, "ERROR: RunAgentThread failed: %d\n", err);
      exit(1);
   }
}

static void JNICALL onClassLoad(jvmtiEnv *jvmti, JNIEnv *jni, jthread thread, jclass clazz)
{
   // AsyncGetCallTrace requires class load events to be enabled
}

static void JNICALL onClassPrepare(jvmtiEnv *jvmti, JNIEnv *jni, jthread thread, jclass clazz)
{
   createMethodIDs(jvmti, clazz);
}

static void JNICALL onThreadStart(jvmtiEnv *jvmti, JNIEnv *jni, jthread thread)
{
   jvmtiError err;

   ThreadTag *tag;
   err = jvmti->Allocate(sizeof(ThreadTag), (unsigned char **) &tag);
   if (!ok(err)) {
      fprintf(stderr, "WARNING: Allocate failed: %d\n", err);
      return;
   }

   tag->jni = jni;
   tag->thread_id = pthread_self();

   err = jvmti->SetTag(thread, (jlong) tag);
   if (!ok(err)) {
      fprintf(stderr, "WARNING: SetTag for thread failed: %d\n", err);
      jvmti->Deallocate((unsigned char *) tag);
   }
}

static void JNICALL onThreadEnd(jvmtiEnv *jvmti, JNIEnv *jni, jthread thread)
{
   jvmti->RawMonitorEnter(x_trace_lock);

   ThreadTag *tag;
   if (ok(jvmti->GetTag(thread, (jlong *) &tag)) && (tag != nullptr)) {
      jvmti->Deallocate((unsigned char *) tag);
      auto err = jvmti->SetTag(thread, 0);
      if (!ok(err)) {
         fprintf(stderr, "WARNING: SetTag for thread failed: %d\n", err);
      }
   }

   jvmti->RawMonitorExit(x_trace_lock);
}

JNIEXPORT jint JNICALL
Agent_OnLoad(JavaVM *vm, char *options, void *reserved)
{
   jvmtiEnv *jvmti;
   jvmtiError err;

   // parse options
   if ((options == nullptr) || (sscanf(options, "port=%d", &port) != 1)) {
      fprintf(stderr, "ERROR: failed to parse port option\n");
      return JNI_ERR;
   }

   // get environment
   int rc = vm->GetEnv((void **) &jvmti, JVMTI_VERSION);
   if (rc != JNI_OK) {
      fprintf(stderr, "ERROR: GetEnv failed: %d\n", rc);
      return JNI_ERR;
   }

   // create lock
   err = jvmti->CreateRawMonitor("astack_trace", &x_trace_lock);
   if (!ok(err)) {
      fprintf(stderr, "ERROR: CreateRawMonitor failed: %d\n", err);
      return JNI_ERR;
   }

   // add capabilities
   jvmtiCapabilities capabilities = {};
   capabilities.can_get_source_file_name = true;
   capabilities.can_get_line_numbers = true;
   capabilities.can_tag_objects = true;

   err = jvmti->AddCapabilities(&capabilities);
   if (!ok(err)) {
      fprintf(stderr, "ERROR: AddCapabilities failed: %d\n", err);
      return JNI_ERR;
   }

   // register event callbacks
   jvmtiEventCallbacks callbacks = {};
   callbacks.VMInit = &onVmInit;
   callbacks.ClassLoad = &onClassLoad;
   callbacks.ClassPrepare = &onClassPrepare;
   callbacks.ThreadStart = &onThreadStart;
   callbacks.ThreadEnd = &onThreadEnd;

   err = jvmti->SetEventCallbacks(&callbacks, sizeof(callbacks));
   if (!ok(err)) {
      fprintf(stderr, "ERROR: SetEventCallbacks failed: %d\n", err);
      return JNI_ERR;
   }

   // request notifications
   jvmtiEvent events[] = {
      JVMTI_EVENT_VM_INIT,
      JVMTI_EVENT_CLASS_LOAD,
      JVMTI_EVENT_CLASS_PREPARE,
      JVMTI_EVENT_THREAD_START,
      JVMTI_EVENT_THREAD_END,
   };

   for (auto event : events) {
      err = jvmti->SetEventNotificationMode(JVMTI_ENABLE, event, nullptr);
      if (!ok(err)) {
         fprintf(stderr, "ERROR: SetEventNotificationMode failed: %d\n", err);
         return JNI_ERR;
      }
   }

   return JNI_OK;
}
