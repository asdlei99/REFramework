#include "REFramework.hpp"

#include "RETypeDB.hpp"

namespace sdk {
RETypeDB* RETypeDB::get() {
    return g_framework->get_types()->get_type_db();
}

static std::shared_mutex g_tdb_type_mtx{};
static std::unordered_map<std::string, sdk::RETypeDefinition*> g_tdb_type_map{};

sdk::InvokeRet invoke_object_func(void* obj, sdk::RETypeDefinition* t, std::string_view name, const std::vector<void*>& args) {
    const auto method = t->get_method(name);

    if (method == nullptr) {
        return InvokeRet{};
    }

    return method->invoke(obj, args);
}

sdk::InvokeRet invoke_object_func(::REManagedObject* obj, std::string_view name, const std::vector<void*>& args) {
   return invoke_object_func((void*)obj, utility::re_managed_object::get_type_definition(obj), name, args);
}

sdk::RETypeDefinition* RETypeDB::find_type(std::string_view name) const {
    {
        std::shared_lock _{ g_tdb_type_mtx };

        if (auto it = g_tdb_type_map.find(name.data()); it != g_tdb_type_map.end()) {
            return it->second;
        }
    }

    for (uint32_t i = 0; i < this->numTypes; ++i) {
        auto t = get_type(i);

        if (t->get_full_name() == name) {
            std::unique_lock _{ g_tdb_type_mtx };

            g_tdb_type_map[name.data()] = t;
            return g_tdb_type_map[name.data()];
        }
    }

    return g_tdb_type_map[name.data()];
}

sdk::RETypeDefinition* RETypeDB::get_type(uint32_t index) const {
    if (index >= this->numTypes) {
        return nullptr;
    }

    return &(*this->types)[index];
}

sdk::REMethodDefinition* RETypeDB::get_method(uint32_t index) const {
    if (index >= this->numMethods) {
        return nullptr;
    }

    return &(*this->methods)[index];
}

sdk::REField* RETypeDB::get_field(uint32_t index) const {
    if (index >= this->numFields) {
        return nullptr;
    }

    return &(*this->fields)[index];
}

sdk::REProperty* RETypeDB::get_property(uint32_t index) const {
    if (index >= this->numProperties) {
        return nullptr;
    }

    return &(*this->properties)[index];
}

const char* RETypeDB::get_string(uint32_t offset) const {
    if (offset >= this->numStringPool) {
        return nullptr;
    }

    return (const char*)((uintptr_t)this->stringPool + offset);
}

uint8_t* RETypeDB::get_bytes(uint32_t offset) const {
    if (offset >= this->numBytePool) {
        return nullptr;
    }

    return (uint8_t*)((uintptr_t)this->bytePool + offset);
}
}

namespace sdk {
sdk::RETypeDefinition* REField::get_declaring_type() const {
    auto tdb = RETypeDB::get();

    if (this->declaring_typeid == 0) {
        return nullptr;
    }

    return tdb->get_type(this->declaring_typeid);
}

sdk::RETypeDefinition* REField::get_type() const {
    auto tdb = RETypeDB::get();

#if TDB_VER >= 69
    auto& impl = (*tdb->fieldsImpl)[this->impl_id];
    const auto field_typeid = (uint32_t)impl.field_typeid;
#else
    const auto field_typeid = (uint32_t)this->field_typeid;
#endif

    if (field_typeid == 0) {
        return nullptr;
    }

    return tdb->get_type(field_typeid);
}

const char* REField::get_name() const {
    auto tdb = RETypeDB::get();

#if TDB_VER >= 69
    auto& impl = (*tdb->fieldsImpl)[this->impl_id];
    const auto name_offset = impl.name_offset;
#else
    const auto name_offset = this->name_offset;
#endif

    return tdb->get_string(name_offset);
}

uint32_t REField::get_flags() const {
    auto tdb = RETypeDB::get();

#if TDB_VER >= 69
    auto& impl = (*tdb->fieldsImpl)[this->impl_id];
    return impl.flags;
#else
    return this->flags;
#endif
}

void* REField::get_init_data() const {
    auto tdb = RETypeDB::get();

#if TDB_VER >= 69
    auto& impl = (*tdb->fieldsImpl)[this->impl_id];
    const auto init_data_index = impl.init_data_lo | (impl.init_data_hi << 14);
    const auto init_data_offset = (*tdb->initData)[init_data_index];
#elif TDB_VER > 49
    const auto init_data_index = this->init_data_index;
    const auto init_data_offset = (*tdb->initData)[init_data_index];
#else
    const auto init_data_offset = this->init_data_offset;
#endif

    auto init_data = tdb->get_bytes(init_data_offset);

    // WACKY
    if (init_data_offset < 0) {
        init_data = (uint8_t*)tdb->get_string(init_data_offset * -1);
    }

    return init_data;
}

uint32_t REField::get_offset_from_fieldptr() const {
    return this->offset;
}

uint32_t REField::get_offset_from_base() const {
    auto offset_from_fieldptr = get_offset_from_fieldptr();
    auto declaring_type = this->get_declaring_type();

    if (declaring_type != nullptr) {
        if (declaring_type->has_fieldptr_offset()) {
            return declaring_type->get_fieldptr_offset() + offset_from_fieldptr;
        }
    }

    return offset_from_fieldptr;
}

bool sdk::REField::is_static() const {
    const auto field_flags = this->get_flags();

    return (field_flags & (uint16_t)via::clr::FieldFlag::Static) != 0;
}

bool sdk::REField::is_literal() const  {
    const auto field_flags = this->get_flags();

    return (field_flags & (uint16_t)via::clr::FieldFlag::Literal) != 0;
}

void* REField::get_data_raw(void* object, bool is_value_type) const {
    const auto field_flags = get_flags();

    if ((field_flags & (uint16_t)via::clr::FieldFlag::Static) != 0) {
        if ((field_flags & (uint16_t)via::clr::FieldFlag::Literal) != 0) {
            return this->get_init_data();
        }

        auto tbl = sdk::VM::get()->get_static_tbl_for_type(this->get_declaring_type()->get_index());

        if (tbl != nullptr) {
            return Address{tbl}.get(this->get_offset_from_fieldptr());
        }
    } else {
        if (is_value_type) {
            return Address{object}.get(this->get_offset_from_fieldptr());
        }

        return Address{object}.get(this->get_offset_from_base());
    }

    return nullptr;
}
} // namespace sdk

// methods
namespace sdk {
sdk::RETypeDefinition* REMethodDefinition::get_declaring_type() const {
    auto tdb = RETypeDB::get();

    if (this->declaring_typeid == 0) {
        return nullptr;
    }

    return tdb->get_type(this->declaring_typeid);
}

sdk::RETypeDefinition* REMethodDefinition::get_return_type() const {
    auto tdb = RETypeDB::get();

#if TDB_VER >= 69
    auto param_ids = tdb->get_data<REParamList>(this->params);
    
    const auto return_param_id = param_ids->returnType;
    const auto& p = (*tdb->params)[return_param_id];

    if (p.type_id == 0) {
        return nullptr;
    }

    const auto return_typeid = p.type_id;
#elif TDB_VER >= 66
    const auto return_typeid = (uint32_t)this->return_typeid;
#else
    const auto return_typeid = tdb->get_data<sdk::REMethodParamDef>(this->params)->return_typeid;
#endif

    if (return_typeid == 0) {
        return nullptr;
    }

    return tdb->get_type(return_typeid);
}

const char* REMethodDefinition::get_name() const {
    auto tdb = RETypeDB::get();

#if TDB_VER >= 69
    auto& impl = (*tdb->methodsImpl)[this->impl_id];
    const auto name_offset = impl.name_offset;
#else
    const auto name_offset = this->name_offset;
#endif

    return tdb->get_string(name_offset);
}

void* REMethodDefinition::get_function() const {
#ifndef RE7
    return this->function;
#else
    auto vm = sdk::VM::get();
    auto& m = vm->methods[this->get_index()];

    return m.function;
#endif
}

uint32_t sdk::REMethodDefinition::get_invoke_id() const {
    auto tdb = RETypeDB::get();

#if TDB_VER >= 69
    const auto param_list = (uint32_t)this->params;
    const auto param_ids = tdb->get_data<REParamList>(param_list);
    const auto num_params = param_ids->numParams;
    const auto invoke_id = param_ids->invokeID;
#else
    const auto invoke_id = (uint16_t)this->invoke_id;
#endif

    return invoke_id;
}

sdk::InvokeRet sdk::REMethodDefinition::invoke(void* object, const std::vector<void*>& args) const {
    if (get_num_params() != args.size()) {
        //throw std::runtime_error("Invalid number of arguments");
        spdlog::warn("Invalid number of arguments passed to REMethodDefinition::invoke for {}", get_name());
        return InvokeRet{};
    }

    const auto invoke_tbl = sdk::get_invoke_table();
    auto invoke_wrapper = invoke_tbl[get_invoke_id()];

    struct StackFrame {
        char pad_0000[8+8]; //0x0000
        const sdk::REMethodDefinition* method;
        char pad_0010[24]; //0x0018
        void* in_data; //0x0030 can point to data
        void* out_data; //0x0038 can be whatever, can be a dword, can point to data
        void* object_ptr; //0x0040 aka "this" pointer
    };

    InvokeRet out{};

    StackFrame stack_frame{};
    stack_frame.method = this;
    stack_frame.object_ptr = object;
    stack_frame.in_data = (void*)args.data();
    
    auto ret_ty = get_return_type();
 
    // vec3 and stuff that is > sizeof(void*) requires special handling
    // by preallocating the output buffer
    if (ret_ty != nullptr && ret_ty->is_value_type()) {
        if (ret_ty->get_valuetype_size() > sizeof(void*) || (!ret_ty->is_primitive() && !ret_ty->is_enum())) {
            stack_frame.out_data = &out;
        } else {
            stack_frame.out_data = nullptr;
        }
    } else {
        stack_frame.out_data = nullptr;
    }
    
    invoke_wrapper((void*)&stack_frame, sdk::get_thread_context());

    if (stack_frame.out_data != &out) {
        out.ptr = stack_frame.out_data;
        return out;
    }

    return out;
}

uint32_t sdk::REMethodDefinition::get_index() const {
    auto tdb = RETypeDB::get();

    return (uint32_t)(((uintptr_t)this - (uintptr_t)tdb->methods) / sizeof(sdk::REMethodDefinition));
}

int32_t REMethodDefinition::get_virtual_index() const {
#if TDB_VER >= 69
    auto tdb = RETypeDB::get();
    auto& impl = (*tdb->methodsImpl)[this->impl_id];
    return impl.vtable_index;
#else
    return this->vtable_index;
#endif
}

uint16_t REMethodDefinition::get_flags() const {
#if TDB_VER >= 69
    auto tdb = RETypeDB::get();
    auto& impl = (*tdb->methodsImpl)[this->impl_id];
    return impl.flags;
#else
    return this->flags;
#endif
}

uint16_t REMethodDefinition::get_impl_flags() const {
#if TDB_VER >= 69
    auto tdb = RETypeDB::get();
    auto& impl = (*tdb->methodsImpl)[this->impl_id];
    return impl.impl_flags;
#else
    return this->impl_flags;
#endif
}

bool REMethodDefinition::is_static() const {
    const auto method_flags = this->get_flags();

    return (method_flags & (uint16_t)via::clr::MethodFlag::Static) != 0;
}

uint32_t sdk::REMethodDefinition::get_num_params() const {
#if TDB_VER >= 69
    auto tdb = RETypeDB::get();
    const auto param_list = (uint32_t)this->params;
    const auto param_ids = tdb->get_data<REParamList>(param_list);
    return param_ids->numParams;
#else
#if TDB_VER >= 66
    return this->num_params;
#else
    auto tdb = RETypeDB::get();
    return tdb->get_data<sdk::REMethodParamDef>(this->params)->num_params;
#endif
#endif
}

std::vector<uint32_t> REMethodDefinition::get_param_typeids() const {
    auto tdb = RETypeDB::get();

    const auto param_list = (uint32_t)this->params;

    std::vector<uint32_t> out{};

    // Parameters
#if TDB_VER >= 69
    auto param_ids = tdb->get_data<REParamList>(param_list);
    const auto num_params = param_ids->numParams;

    // Parse all params
    for (auto f = 0; f < num_params; ++f) {
        const auto param_index = param_ids->params[f];

        if (param_index >= tdb->numParams) {
            break;
        }

        auto& p = (*tdb->params)[param_index];

        out.push_back(p.type_id);
    }

    return out;
#else
#if TDB_VER >= 66
    const auto param_ids = tdb->get_data<sdk::REMethodParamDef>(param_list);
#else
    const auto param_data = tdb->get_data<sdk::REMethodParamDef>(param_list);
    const auto param_ids = param_data->params;
#endif

    const auto num_params = get_num_params();

    for (uint32_t f = 0; f < num_params; ++f) {
        auto& p = param_ids[f];
        out.push_back((uint32_t)p.param_typeid);
    }

    return out;
#endif
}

std::vector<sdk::RETypeDefinition*> REMethodDefinition::get_param_types() const {
    auto typeids = get_param_typeids();

    if (typeids.empty()) {
        return {};
    }

    auto tdb = RETypeDB::get();
    std::vector<sdk::RETypeDefinition*> out{};

    for (auto id : typeids) {
        out.push_back(tdb->get_type(id));
    }

    return out;
}

std::vector<const char*> REMethodDefinition::get_param_names() const {

    std::vector<const char*> out{};

    auto tdb = RETypeDB::get();
    const auto param_list = (uint32_t)this->params;

#if TDB_VER >= 69
    auto param_ids = tdb->get_data<REParamList>(param_list);
    const auto num_params = param_ids->numParams;

    // Parse all params
    for (auto f = 0; f < num_params; ++f) {
        const auto param_index = param_ids->params[f];

        if (param_index >= tdb->numParams) {
            break;
        }

        auto& p = (*tdb->params)[param_index];

        out.push_back(tdb->get_string(p.name_offset));
    }

    return out;
#else
    const auto num_params = get_num_params();

#if TDB_VER >= 66
    const auto param_ids = tdb->get_data<sdk::REMethodParamDef>(param_list);
#else
    const auto param_data = tdb->get_data<sdk::REMethodParamDef>(param_list);
    const auto param_ids = param_data->params;
#endif

    for (uint32_t f = 0; f < num_params; ++f) {
        const auto param_index = param_ids[f].param_typeid;
        const auto name_offset = (uint32_t)param_ids[f].name_offset;
        out.push_back(tdb->get_string(name_offset));
    }
    
    return out;
#endif
}
} // namespace sdk