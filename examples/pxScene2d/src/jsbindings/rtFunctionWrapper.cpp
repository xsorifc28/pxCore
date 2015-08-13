#include "rtFunctionWrapper.h"
#include "rtWrapperUtils.h"
#include "jsCallback.h"

#include <vector>

static const char* kClassName = "Function";
static Persistent<Function> ctor;

static void jsFunctionCompletionHandler(void* argp, rtValue const& result)
{
  jsFunctionWrapper* wrapper = reinterpret_cast<jsFunctionWrapper *>(argp);
  wrapper->signal(result);
}

class ResolverFunction : public rtAbstractFunction
{
public:
  enum Disposition
  {
    DispositionResolve,
    DispositionReject
  };

  ResolverFunction(Disposition disp, Isolate* isolate, Local<Promise::Resolver>& resolver)
    : rtAbstractFunction()
    , mDisposition(disp)
    , mResolver(isolate, resolver)
    , mIsolate(isolate)
  {
  }

  virtual ~ResolverFunction()
  {
    rtLogInfo("delete");
  }

  virtual rtError Send(int numArgs, const rtValue* args, rtValue* /*result*/)
  {
    AsyncContext* ctx = new AsyncContext();

    // keep current object alive while we enqueue this request
    ctx->resolverFunc = rtFunctionRef(this);

    for (int i = 0; i < numArgs; ++i)
      ctx->args.push_back(args[i]);

    mReq.data = ctx;
    uv_queue_work(uv_default_loop(), &mReq, &workCallback, &afterWorkCallback);
    return RT_OK;
  }

private:
  struct AsyncContext
  {
    rtFunctionRef resolverFunc;
    std::vector<rtValue> args;
  };

  static void workCallback(uv_work_t* /*req */)
  {
    // empty
  }

  static void afterWorkCallback(uv_work_t* req, int /* status */)
  {
    AsyncContext* ctx = reinterpret_cast<AsyncContext*>(req->data);
    ResolverFunction* resolverFunc = static_cast<ResolverFunction *>(ctx->resolverFunc.getPtr());

    assert(ctx->args.size() < 2);
    assert(Isolate::GetCurrent() == resolverFunc->mIsolate);

    // Locker locker(resolverFunc->mIsolate);
    HandleScope scope(resolverFunc->mIsolate);
    // Context::Scope contextScope(PersistentToLocal(resolverFunc->mIsolate, resolverFunc->mContext));

    Handle<Value> value;
    if (ctx->args.size() > 0)
      value = rt2js(resolverFunc->mIsolate, ctx->args[0]);

    Local<Promise::Resolver> resolver = PersistentToLocal(resolverFunc->mIsolate, resolverFunc->mResolver);

    TryCatch tryCatch;
    if (resolverFunc->mDisposition == DispositionResolve)
      resolver->Resolve(value);
    else
      resolver->Reject(value);

    if (tryCatch.HasCaught())
    {
      String::Utf8Value trace(tryCatch.StackTrace());
      rtLogWarn("Error resolving promise");
      rtLogWarn("%s", *trace);
    }

    resolverFunc->mIsolate->RunMicrotasks();

    delete ctx;
  }

private:
  Disposition mDisposition;
  Persistent<Promise::Resolver> mResolver;
  Isolate* mIsolate;
  uv_work_t mReq;
};

static bool isPromise(const rtValue& v)
{
  if (v.getType() != RT_rtObjectRefType)
    return false;

  rtObjectRef ref = v.toObject();
  if (!ref)
    return false;

  rtString desc;
  rtError err = ref.sendReturns<rtString>("description", desc);
  return err == RT_OK && strcmp(desc.cString(), "rtPromise") == 0;
}

rtFunctionWrapper::rtFunctionWrapper(const rtFunctionRef& ref)
  : rtWrapper(ref)
{
}

rtFunctionWrapper::~rtFunctionWrapper()
{
}

void rtFunctionWrapper::exportPrototype(Isolate* isolate, Handle<Object> exports)
{
  Local<FunctionTemplate> tmpl = FunctionTemplate::New(isolate, create);
  tmpl->SetClassName(String::NewFromUtf8(isolate, kClassName));

  Local<ObjectTemplate> inst = tmpl->InstanceTemplate();
  inst->SetInternalFieldCount(1);
  inst->SetCallAsFunctionHandler(call);

  ctor.Reset(isolate, tmpl->GetFunction());
  exports->Set(String::NewFromUtf8(isolate, kClassName), tmpl->GetFunction());
}

void rtFunctionWrapper::create(const FunctionCallbackInfo<Value>& args)
{ 
  assert(args.IsConstructCall());

  HandleScope scope(args.GetIsolate());
  rtIFunction* func = static_cast<rtIFunction *>(args[0].As<External>()->Value());
  rtFunctionWrapper* wrapper = new rtFunctionWrapper(func);
  wrapper->Wrap(args.This());
}

Handle<Object> rtFunctionWrapper::createFromFunctionReference(Isolate* isolate, const rtFunctionRef& func)
{
  EscapableHandleScope scope(isolate);
  Local<Value> argv[1] = 
  {
    External::New(isolate, func.getPtr()) 
  };
  Local<Function> c = PersistentToLocal(isolate, ctor);
  return scope.Escape(c->NewInstance(1, argv));
}

void rtFunctionWrapper::call(const FunctionCallbackInfo<Value>& args)
{
  Isolate* isolate = args.GetIsolate();

  HandleScope scope(isolate);

  rtWrapperError error;

  std::vector<rtValue> argList;
  for (int i = 0; i < args.Length(); ++i)
  {
    argList.push_back(js2rt(isolate, args[i], &error));
    if (error.hasError())
      isolate->ThrowException(error.toTypeError(isolate));
  }

  rtValue result;
  rtWrapperSceneUpdateEnter();
  rtError err = unwrap(args)->Send(args.Length(), &argList[0], &result);

  if (err != RT_OK)
  {
    rtWrapperSceneUpdateExit();
    return throwRtError(isolate, err, "failed to invoke function");
  }

  if (isPromise(result))
  {
    Local<Promise::Resolver> resolver = Promise::Resolver::New(isolate);

    rtFunctionRef resolve(new ResolverFunction(ResolverFunction::DispositionResolve, isolate, resolver));
    rtFunctionRef reject(new ResolverFunction(ResolverFunction::DispositionReject, isolate, resolver));
    rtObjectRef newPromise;

    rtObjectRef promise = result.toObject();
    rtError err = promise.send("then", resolve, reject, newPromise);

    // must hold this lock to prevent promise from resolving internally before we
    // actually register our function callbacks.
    rtWrapperSceneUpdateExit();

    if (err != RT_OK)
      return throwRtError(isolate, err, "failed to register for promise callback");
    else
      args.GetReturnValue().Set(resolver->GetPromise());
  }
  else
  {
    rtWrapperSceneUpdateExit();
    args.GetReturnValue().Set(rt2js(isolate, result));
  }
}

jsFunctionWrapper::jsFunctionWrapper(Isolate* isolate, const Handle<Value>& val)
  : mRefCount(0)
  , mFunction(isolate, Handle<Function>::Cast(val))
  , mIsolate(isolate)
  , mComplete(false)
  , mTeardownThreadingPrimitives(false)
{
  assert(val->IsFunction());
}

jsFunctionWrapper::~jsFunctionWrapper()
{
  if (mTeardownThreadingPrimitives)
  {
    pthread_mutex_destroy(&mMutex);
    pthread_cond_destroy(&mCond);
  }
}

void jsFunctionWrapper::setupSynchronousWait()
{
  mComplete = false;
  mTeardownThreadingPrimitives = true;
  pthread_mutex_init(&mMutex, NULL);
  pthread_cond_init(&mCond, NULL);
}

void jsFunctionWrapper::signal(rtValue const& returnValue)
{
  pthread_mutex_lock(&mMutex);
  mComplete = true;
  mReturnValue = returnValue;
  pthread_mutex_unlock(&mMutex);
  pthread_cond_signal(&mCond);
}

rtValue jsFunctionWrapper::wait()
{
  pthread_mutex_lock(&mMutex);
  while (!mComplete)
    pthread_cond_wait(&mCond, &mMutex);
  pthread_mutex_unlock(&mMutex);
  return mReturnValue;
}

unsigned long jsFunctionWrapper::AddRef()
{
  return rtAtomicInc(&mRefCount);
}

unsigned long jsFunctionWrapper::Release()
{
  unsigned long l = rtAtomicDec(&mRefCount);
  if (l == 0) delete this;
  return l;
}

rtError jsFunctionWrapper::Send(int numArgs, const rtValue* args, rtValue* result)
{
  //
  // TODO: Return values are not supported. This class is an rtFunction that wraps
  // a javascript function. If everything is behaving normally, we're running in the
  // context of a native/non-js thread. This is almost certainly the "render" thread.
  // The function is "packed" up and sent off to a javascript thread via the
  // enqueue() on the jsCallback. That means the called is queued with nodejs' event
  // queue. This is required to prevent multiple threads from
  // entering the JS engine. The problem is that the caller can't expect anything in
  // return in the result (last parameter to this function.
  // If you have the current thread wait and then return the result, you'd block this
  // thread until the completion of the javascript function call.
  //
  // Here's an example of how you'd get into this situation. This is a contrived example.
  // No known case exists right now.
  //
  // The closure function below will be wrapped and registered with the rt object layer.
  // If the 'someEvent' is fired, excecution wll arrive here (this code). You'll never see
  // the "return true" because this call returns and the function is run in another
  // thread.
  //
  // This won't work!
  //
  // var foo = ...
  // foo.on('someEvent', function(msg) {
  //    console.log("I'm running in a javascript thread");
  //    return true; // <-- Can't do this
  // });
  //
  jsCallback* callback = jsCallback::create(mIsolate);
  for (int i = 0; i < numArgs; ++i)
    callback->addArg(args[i]);

  callback->setFunctionLookup(new FunctionLookup(this));

  if (result) // wants result run synchronously
  {
    if (rtIsMainThread()) // main thread run now
    {
      *result = callback->run();
    }
    else // queue and wait
    {
      setupSynchronousWait();
      callback->registerForCompletion(jsFunctionCompletionHandler, this);
      callback->enqueue();

      // don't block render thread while waiting for callback to complete
      rtWrapperSceneUnlocker unlocker;

      *result = wait();
    }
  }
  else // just queue
  {
    callback->enqueue();
  }

  return RT_OK;
}

Local<Function> jsFunctionWrapper::FunctionLookup::lookup()
{
  return PersistentToLocal(mParent->mIsolate, mParent->mFunction);
}

