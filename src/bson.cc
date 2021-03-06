#ifdef _WIN32
#pragma warning(disable : 4996)
#endif

#include "bson.h"

#include <nan.h>
#include <string.h>

typedef struct object_wrapper_s
{
    v8::Local<v8::Object> object;
    uint32_t index;
    object_wrapper_s *next;

    object_wrapper_s(v8::Local<v8::Object> obj, object_wrapper_s *curr) : object(obj), index(curr ? curr->index + 1 : 0), next(curr) {}
} object_wrapper_t;

typedef struct writer_s
{
    bool deleteOld;
    size_t capacity;
    size_t used;
    uint8_t *current;

    object_wrapper_t *objects;

    inline writer_s(bson::BSONValue &value) : deleteOld(false), capacity(sizeof(value.cache)), used(0), current(value.cache), objects(NULL) {}

    inline void ensureCapacity(size_t required)
    {
        required += used;
        if (capacity >= required)
        {
            used = required;
            return;
        }

        if (!deleteOld)
        {
            capacity = 4096;
        }
        while (capacity < required)
        {
            capacity <<= 1;
        }

        uint8_t *new_pointer = new uint8_t[capacity];
        // fprintf(stderr, "writer::ensureCapacity: new buffer allocated:%x (len:%d, required:%d, used:%d)\n", new_pointer, capacity, required, used);
        uint8_t *old_pointer = current - used;
        memcpy(new_pointer, old_pointer, used);

        if (deleteOld)
            delete[] old_pointer;
        else
            deleteOld = true;

        current = new_pointer + used;

        // set used to required + used
        used = required;
    }

    inline ~writer_s()
    {
        object_wrapper_t *curr = objects;
        while (curr)
        {
            object_wrapper_t *next = curr->next;
            delete curr;
            curr = next;
        }
    }

    void write(v8::Local<v8::Value> value)
    {
        using namespace v8;
        ensureCapacity(1);
        if (value->IsString())
        {
            *(current++) = bson::String;
            Local<String> str = value->ToString(Nan::GetCurrentContext()).ToLocalChecked();
            size_t len = str->Utf8Length(Nan::GetCurrentContext()->GetIsolate()) + 1;
            ensureCapacity(sizeof(uint32_t) + len);
            *reinterpret_cast<uint32_t *>(current) = len;
            current += sizeof(uint32_t);
            str->WriteUtf8(Nan::GetCurrentContext()->GetIsolate(), reinterpret_cast<char *>(current), -1, nullptr, 0);
            current += len;
        }
        else if (value->IsNull())
        {
            *(current++) = bson::Null;
        }
        else if (value->IsBoolean())
        {
            *(current++) = value->IsTrue() ? bson::True : bson::False;
        }
        else if (value->IsInt32())
        {
            *(current++) = bson::Int32;
            ensureCapacity(sizeof(int32_t));
            *reinterpret_cast<int32_t *>(current) = value->Int32Value(Nan::GetCurrentContext()).ToChecked();
            current += sizeof(int32_t);
        }
        else if (value->IsNumber())
        {
            *(current++) = bson::Number;
            ensureCapacity(sizeof(double));
            *reinterpret_cast<double *>(current) = value->NumberValue(Nan::GetCurrentContext()).ToChecked();
            current += sizeof(double);
        }
        else if (value->IsObject())
        {
            Local<Object> obj = value.As<Object>();
            // check if object has already been serialized
            object_wrapper_t *curr = objects;
            while (curr)
            {
                if (curr->object->StrictEquals(value))
                { // found
                    *(current++) = bson::ObjectRef;
                    ensureCapacity(sizeof(uint32_t));
                    *reinterpret_cast<uint32_t *>(current) = curr->index;
                    current += sizeof(uint32_t);
                    return;
                }
                curr = curr->next;
            }
            // curr is null
            objects = new object_wrapper_t(obj, objects);
            if (value->IsArray())
            {
                *(current++) = bson::Array;
                Local<Array> arr = obj.As<Array>();
                uint32_t len = arr->Length();
                ensureCapacity(sizeof(uint32_t));
                *reinterpret_cast<uint32_t *>(current) = len;
                current += sizeof(uint32_t);
                for (uint32_t i = 0; i < len; i++)
                {
                    // fprintf(stderr, "write array[%d] (len=%d)\n", i, len);
                    write(Nan::Get(arr, i).ToLocalChecked());
                }
            }
            else if (node::Buffer::HasInstance(value))
            {
                *(current++) = bson::Buffer;
                char *data = node::Buffer::Data(value);
                uint32_t len = node::Buffer::Length(value);
                ensureCapacity(sizeof(uint32_t) + len);
                *reinterpret_cast<uint32_t *>(current) = len;
                current += sizeof(uint32_t);
                for (uint32_t i = 0; i < len; i++)
                {
                    *(current++) = data[i];
                }
            }
            else
            { // TODO: support for other object types
                *(current++) = bson::Object;
                Local<Array> names = obj->GetOwnPropertyNames(Nan::GetCurrentContext()).ToLocalChecked();
                uint32_t len = names->Length();
                ensureCapacity(sizeof(uint32_t));
                *reinterpret_cast<uint32_t *>(current) = len;
                current += sizeof(uint32_t);
                for (uint32_t i = 0; i < len; i++)
                {
                    Local<Value> name = Nan::Get(names, i).ToLocalChecked();
                    write(name);
                    write(Nan::Get(obj, name).ToLocalChecked());
                }
            }
        }
        else
        {
            *(current++) = bson::Undefined;
        }
    }

} writer_t;

bson::BSONValue::BSONValue(v8::Local<v8::Value> value)
{

    writer_t writer(*this);
    writer.write(value);
    // fprintf(stderr, "%d bytes used writing %s\n", writer.used, *Nan::Utf8String(value));

    pointer = writer.current - writer.used;
    length = writer.used;
}

bson::BSONValue::~BSONValue()
{
    if (length > sizeof(cache))
    { // new cache is allocated
        // fprintf(stderr, "BSONValue: freeing buffer %x (len:%d)\n", pointer, length);
        delete[] pointer;
    }
}

static v8::Local<v8::Value> parse(const uint8_t *&data, object_wrapper_t *&objects)
{
    using namespace v8;
    uint32_t len;
    const uint8_t *tmp;
    switch (*(data++))
    {
    case bson::Null:
        return Nan::Null();
    case bson::Undefined:
        return Nan::Undefined();
    case bson::True:
        return Nan::True();
    case bson::False:
        return Nan::False();
    case bson::Int32:
        tmp = data;
        data += sizeof(int32_t);
        return Nan::New<Integer>(*reinterpret_cast<const int32_t *>(tmp));
    case bson::Number:
        tmp = data;
        data += sizeof(double);
        return Nan::New<Number>(*reinterpret_cast<const double *>(tmp));
    case bson::String:
        len = *reinterpret_cast<const uint32_t *>(data);
        tmp = data += sizeof(uint32_t);
        data += len;
        return v8::String::NewFromUtf8(Isolate::GetCurrent(), reinterpret_cast<const char *>(tmp), v8::NewStringType::kNormal, len - 1).ToLocalChecked();
    case bson::Array:
        len = *reinterpret_cast<const uint32_t *>(data);
        data += sizeof(uint32_t);
        {
            Local<Array> arr = Nan::New<Array>(len);
            objects = new object_wrapper_t(arr, objects);

            for (uint32_t i = 0; i < len; i++)
            {
                Nan::Set(arr, i, parse(data, objects));
            }
            return arr;
        }
    case bson::Object:
        len = *reinterpret_cast<const uint32_t *>(data);
        data += sizeof(uint32_t);
        {
            Local<Object> obj = Nan::New<Object>();
            objects = new object_wrapper_t(obj, objects);

            for (uint32_t i = 0; i < len; i++)
            {
                Local<Value> name = parse(data, objects);
                Nan::Set(obj, name, parse(data, objects));
            }
            return obj;
        }
    case bson::ObjectRef:
        len = *reinterpret_cast<const uint32_t *>(data);
        data += sizeof(uint32_t);
        {
            object_wrapper_t *curr = objects;
            while (curr->index != len)
            {
                curr = curr->next;
            }
            return curr->object;
        }
    case bson::Buffer:
        len = *reinterpret_cast<const uint32_t *>(data);
        data += sizeof(uint32_t);
        {
            char *retval = new char[len];
            for (uint32_t i = 0; i < len; i++)
            {
                retval[i] = data[i];
            }
            Local<Object> obj = Nan::NewBuffer(retval, len).ToLocalChecked();
            data += len;
            return obj;
        }
    }
    assert("should not reach here");
    return Local<Value>();
}

v8::Local<v8::Value> bson::parse(const uint8_t *data)
{
    object_wrapper_t *objects = NULL;
    v8::Local<v8::Value> ret = ::parse(data, objects);
    while (objects)
    {
        object_wrapper_t *next = objects->next;
        delete objects;
        objects = next;
    }

    return ret;
}
