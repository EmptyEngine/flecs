#include "meta.h"

#ifdef FLECS_META

/* EcsMetaTypeSerialized lifecycle */

static ECS_COPY(EcsMetaTypeSerialized, dst, src, {
    ecs_vector_free(dst->ops);
    dst->ops = ecs_vector_copy(src->ops, ecs_meta_type_op_t);
})

static ECS_MOVE(EcsMetaTypeSerialized, dst, src, {
    ecs_vector_free(dst->ops);
    dst->ops = src->ops;
    src->ops = NULL;
})

static ECS_DTOR(EcsMetaTypeSerialized, ptr, { 
    ecs_vector_free(ptr->ops); 
})


/* EcsStruct lifecycle */

static void dtor_struct(
    EcsStruct *ptr) 
{
    ecs_member_t *members = ecs_vector_first(ptr->members, ecs_member_t);
    int32_t i, count = ecs_vector_count(ptr->members);
    for (i = 0; i < count; i ++) {
        ecs_os_free(members[i].name);
    }
    ecs_vector_free(ptr->members);
}

static ECS_COPY(EcsStruct, dst, src, {
    dtor_struct(dst);

    dst->members = ecs_vector_copy(src->members, ecs_member_t);

    ecs_member_t *members = ecs_vector_first(dst->members, ecs_member_t);
    int32_t m, count = ecs_vector_count(dst->members);

    for (m = 0; m < count; m ++) {
        members[m].name = ecs_os_strdup(members[m].name);
    }
})

static ECS_MOVE(EcsStruct, dst, src, {
    dtor_struct(dst);
    dst->members = src->members;
    src->members = NULL;
})

static ECS_DTOR(EcsStruct, ptr, { dtor_struct(ptr); })

static
int init_type(
    ecs_world_t *world,
    ecs_entity_t type,
    ecs_type_kind_t kind)
{
    ecs_assert(world != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(type != 0, ECS_INTERNAL_ERROR, NULL);

    EcsMetaType *meta_type = ecs_get_mut(world, type, EcsMetaType, NULL);
    if (meta_type->kind && meta_type->kind != kind) {
        ecs_err("type '%s' reregistered with different kind", 
            ecs_get_name(world, type));
        return -1;
    }

    meta_type->kind = kind;
    ecs_modified(world, type, EcsMetaType);

    return 0;
}

static
int init_component(
    ecs_world_t *world,
    ecs_entity_t type,
    ecs_size_t size,
    ecs_size_t alignment)
{
    ecs_assert(world != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(type != 0, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(size != 0, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(alignment != 0, ECS_INTERNAL_ERROR, NULL);

    EcsComponent *component = ecs_get_mut(world, type, EcsComponent, NULL);
    if (component->size && component->size != size) {
        ecs_err("type '%s' reregistered with different size",
            ecs_get_name(world, type));
        return -1;
    }

    if (component->alignment && component->alignment != alignment) {
        ecs_err("type '%s' reregistered with different alignment",
            ecs_get_name(world, type));
        return -1;
    }

    component->size = size;
    component->alignment = alignment;
    ecs_modified(world, type, EcsComponent);

    return 0;
}

static
int add_member_to_struct(
    ecs_world_t *world,
    ecs_entity_t type,
    ecs_entity_t member,
    EcsMember *m)
{
    ecs_assert(world != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(type != 0, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(member != 0, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(m != NULL, ECS_INTERNAL_ERROR, NULL);

    EcsStruct *s = ecs_get_mut(world, type, EcsStruct, NULL);
    ecs_assert(s != NULL, ECS_INTERNAL_ERROR, NULL);

    /* First check if member is already added to struct */
    ecs_member_t *members = ecs_vector_first(s->members, ecs_member_t);
    int32_t i, count = ecs_vector_count(s->members);
    for (i = 0; i < count; i ++) {
        if (members[i].member == member) {
            members[i].type = m->type;
        }
    }

    /* If member wasn't added yet, add a new element to vector */
    if (i == count) {
        ecs_member_t *elem = ecs_vector_add(&s->members, ecs_member_t);
        elem->member = member;
        elem->type = m->type;

        /* Reobtain members array in case it was reallocated */
        members = ecs_vector_first(s->members, ecs_member_t);
        count ++;
    }

    /* Compute member offsets and size & alignment of struct */
    ecs_size_t size = 0;
    ecs_size_t alignment = 0;

    for (i = 0; i < count; i ++) {
        ecs_member_t *elem = &members[i];

        /* Get component of member type to get its size & alignment */
        const EcsComponent *mbr_comp = ecs_get(world, elem->type, EcsComponent);
        if (!mbr_comp) {
            char *path = ecs_get_fullpath(world, member);
            ecs_err("member '%s' is not a type", path);
            ecs_os_free(path);
            return -1;
        }

        ecs_size_t member_size = mbr_comp->size;
        ecs_size_t member_alignment = mbr_comp->alignment;

        if (!member_size || !member_alignment) {
            char *path = ecs_get_fullpath(world, member);
            ecs_err("member '%s' has 0 size/alignment");
            ecs_os_free(path);
            return -1;
        }

        /* Only assign name if this is the new member */
        if (elem->member == member) {
            elem->name = ecs_os_strdup(ecs_get_name(world, member));

            elem->count = m->count;
            if (!elem->count) {
                elem->count = 1;
            }
        }

        member_size *= elem->count;
        size = ECS_ALIGN(size, member_alignment);
        elem->size = member_size;
        elem->offset = size;

        size += member_size;

        if (member_alignment > alignment) {
            alignment = member_alignment;
        }
    }

    if (size == 0) {
        ecs_err("struct '%s' has 0 size", ecs_get_name(world, type));
        return -1;
    }

    if (alignment == 0) {
        ecs_err("struct '%s' has 0 alignment", ecs_get_name(world, type));
        return -1;
    }

    /* Align struct size to struct alignment */
    size = ECS_ALIGN(size, alignment);

    ecs_modified(world, type, EcsStruct);

    /* Overwrite component size & alignment */
    EcsComponent *comp = ecs_get_mut(world, type, EcsComponent, NULL);
    comp->size = size;
    comp->alignment = alignment;
    ecs_modified(world, type, EcsComponent);

    /* Do this last as it triggers the update of EcsMetaTypeSerialized */
    if (init_type(world, type, EcsStructType)) {
        return -1;
    }

    return 0;
}

static
void set_primitive(ecs_iter_t *it) {
    ecs_world_t *world = it->world;
    EcsPrimitive *type = ecs_term(it, EcsPrimitive, 1);

    int i, count = it->count;
    for (i = 0; i < count; i ++) {
        ecs_entity_t e = it->entities[i];
        switch(type->kind) {
        case EcsBool:
            init_component(world, e, 
                ECS_SIZEOF(bool), ECS_ALIGNOF(bool));
            init_type(world, e, EcsPrimitiveType);
            break;
        case EcsChar:
            init_component(world, e, 
                ECS_SIZEOF(char), ECS_ALIGNOF(char));
            init_type(world, e, EcsPrimitiveType);
            break;
        case EcsByte:
            init_component(world, e, 
                ECS_SIZEOF(bool), ECS_ALIGNOF(bool));
            init_type(world, e, EcsPrimitiveType);
            break;
        case EcsU8:
            init_component(world, e, 
                ECS_SIZEOF(uint8_t), ECS_ALIGNOF(uint8_t));
            init_type(world, e, EcsPrimitiveType);
            break;
        case EcsU16:
            init_component(world, e, 
                ECS_SIZEOF(uint16_t), ECS_ALIGNOF(uint16_t));
            init_type(world, e, EcsPrimitiveType);
            break;
        case EcsU32:
            init_component(world, e, 
                ECS_SIZEOF(uint32_t), ECS_ALIGNOF(uint32_t));
            init_type(world, e, EcsPrimitiveType);
            break;
        case EcsU64:
            init_component(world, e, 
                ECS_SIZEOF(uint64_t), ECS_ALIGNOF(uint64_t));
            init_type(world, e, EcsPrimitiveType);
            break;
        case EcsI8:
            init_component(world, e, 
                ECS_SIZEOF(int8_t), ECS_ALIGNOF(int8_t));
            init_type(world, e, EcsPrimitiveType);
            break;
        case EcsI16:
            init_component(world, e, 
                ECS_SIZEOF(int16_t), ECS_ALIGNOF(int16_t));
            init_type(world, e, EcsPrimitiveType);
            break;
        case EcsI32:
            init_component(world, e, 
                ECS_SIZEOF(int32_t), ECS_ALIGNOF(int32_t));
            init_type(world, e, EcsPrimitiveType);
            break;
        case EcsI64:
            init_component(world, e, 
                ECS_SIZEOF(int64_t), ECS_ALIGNOF(int64_t));
            init_type(world, e, EcsPrimitiveType);
            break;
        case EcsF32:
            init_component(world, e, 
                ECS_SIZEOF(float), ECS_ALIGNOF(float));
            init_type(world, e, EcsPrimitiveType);
            break;
        case EcsF64:
            init_component(world, e, 
                ECS_SIZEOF(double), ECS_ALIGNOF(double));
            init_type(world, e, EcsPrimitiveType);
            break;
        case EcsUPtr:
            init_component(world, e, 
                ECS_SIZEOF(uintptr_t), ECS_ALIGNOF(uintptr_t));
            init_type(world, e, EcsPrimitiveType);
            break;
        case EcsIPtr:
            init_component(world, e, 
                ECS_SIZEOF(intptr_t), ECS_ALIGNOF(intptr_t));
            init_type(world, e, EcsPrimitiveType);
            break;
        case EcsString:
            init_component(world, e, 
                ECS_SIZEOF(char*), ECS_ALIGNOF(char*));
            init_type(world, e, EcsPrimitiveType);
            break;
        case EcsEntity:
            init_component(world, e, 
                ECS_SIZEOF(ecs_entity_t), ECS_ALIGNOF(ecs_entity_t));
            init_type(world, e, EcsPrimitiveType);
            break;
        }
    }
}

static
void set_member(ecs_iter_t *it) {
    ecs_world_t *world = it->world;
    EcsMember *member = ecs_term(it, EcsMember, 1);

    int i, count = it->count;
    for (i = 0; i < count; i ++) {
        ecs_entity_t e = it->entities[i];
        ecs_entity_t parent = ecs_get_object(world, e, EcsChildOf, 0);
        if (!parent) {
            ecs_err("missing parent for member '%s'", ecs_get_name(world, e));
            continue;
        }

        add_member_to_struct(world, parent, e, member);
    }
}

static
void set_array(ecs_iter_t *it) {
    ecs_world_t *world = it->world;
    EcsArray *array = ecs_term(it, EcsArray, 1);

    int i, count = it->count;
    for (i = 0; i < count; i ++) {
        ecs_entity_t e = it->entities[i];
        ecs_entity_t elem_type = array[i].type;
        int32_t elem_count = array[i].count;

        if (!elem_type) {
            ecs_err("array '%s' has no element type", ecs_get_name(world, e));
            continue;
        }

        if (!elem_count) {
            ecs_err("array '%s' has size 0", ecs_get_name(world, e));
            continue;
        }

        const EcsComponent *elem_ptr = ecs_get(world, elem_type, EcsComponent);
        if (init_component(
            world, e, elem_ptr->size * elem_count, elem_ptr->alignment))
        {
            continue;
        }

        if (init_type(world, e, EcsArrayType)) {
            continue;
        }
    }
}

static
void set_vector(ecs_iter_t *it) {
    ecs_world_t *world = it->world;
    EcsVector *array = ecs_term(it, EcsVector, 1);

    int i, count = it->count;
    for (i = 0; i < count; i ++) {
        ecs_entity_t e = it->entities[i];
        ecs_entity_t elem_type = array[i].type;

        if (!elem_type) {
            ecs_err("vector '%s' has no element type", ecs_get_name(world, e));
            continue;
        }

        if (init_component(world, e, 
            ECS_SIZEOF(ecs_vector_t*), ECS_ALIGNOF(ecs_vector_t*)))
        {
            continue;
        }

        if (init_type(world, e, EcsVectorType)) {
            continue;
        }
    }
}

void FlecsMetaImport(
    ecs_world_t *world)
{
    ECS_MODULE(world, FlecsMeta);

    ecs_set_name_prefix(world, "Ecs");

    flecs_bootstrap_component(world, EcsMetaType);
    flecs_bootstrap_component(world, EcsMetaTypeSerialized);
    flecs_bootstrap_component(world, EcsPrimitive);
    flecs_bootstrap_component(world, EcsEnum);
    flecs_bootstrap_component(world, EcsBitmask);
    flecs_bootstrap_component(world, EcsMember);
    flecs_bootstrap_component(world, EcsStruct);
    flecs_bootstrap_component(world, EcsArray);
    flecs_bootstrap_component(world, EcsVector);

    ecs_set_component_actions(world, EcsMetaType, { .ctor = ecs_default_ctor });

    ecs_set_component_actions(world, EcsMetaTypeSerialized, { 
        .ctor = ecs_default_ctor,
        .move = ecs_move(EcsMetaTypeSerialized),
        .copy = ecs_copy(EcsMetaTypeSerialized),
        .dtor = ecs_dtor(EcsMetaTypeSerialized)
    });

    ecs_set_component_actions(world, EcsStruct, { 
        .ctor = ecs_default_ctor,
        .move = ecs_move(EcsStruct),
        .copy = ecs_copy(EcsStruct),
        .dtor = ecs_dtor(EcsStruct)
    });

    /* Register triggers to finalize type information from component data */
    ecs_trigger_init(world, &(ecs_trigger_desc_t) {
        .term.id = ecs_id(EcsPrimitive),
        .events = {EcsOnSet},
        .callback = set_primitive
    });

    ecs_trigger_init(world, &(ecs_trigger_desc_t) {
        .term.id = ecs_id(EcsMember),
        .events = {EcsOnSet},
        .callback = set_member
    });

    ecs_trigger_init(world, &(ecs_trigger_desc_t) {
        .term.id = ecs_id(EcsArray),
        .events = {EcsOnSet},
        .callback = set_array
    });

    ecs_trigger_init(world, &(ecs_trigger_desc_t) {
        .term.id = ecs_id(EcsVector),
        .events = {EcsOnSet},
        .callback = set_vector
    });

    ecs_trigger_init(world, &(ecs_trigger_desc_t) {
        .term.id = ecs_id(EcsMetaType),
        .events = {EcsOnSet},
        .callback = ecs_meta_type_serialized_init
    });

    /* Initialize primitive types */
    #define ECS_PRIMITIVE(world, type, primitive_kind)\
        ecs_entity_init(world, &(ecs_entity_desc_t) {\
            .entity = ecs_id(ecs_##type##_t),\
            .name = #type });\
        ecs_set(world, ecs_id(ecs_##type##_t), EcsPrimitive, {\
            .kind = primitive_kind\
        });

    ECS_PRIMITIVE(world, bool, EcsBool);
    ECS_PRIMITIVE(world, char, EcsChar);
    ECS_PRIMITIVE(world, byte, EcsByte);
    ECS_PRIMITIVE(world, u8, EcsU8);
    ECS_PRIMITIVE(world, u16, EcsU16);
    ECS_PRIMITIVE(world, u32, EcsU32);
    ECS_PRIMITIVE(world, u64, EcsU64);
    ECS_PRIMITIVE(world, uptr, EcsUPtr);
    ECS_PRIMITIVE(world, i8, EcsI8);
    ECS_PRIMITIVE(world, i16, EcsI16);
    ECS_PRIMITIVE(world, i32, EcsI32);
    ECS_PRIMITIVE(world, i64, EcsI64);
    ECS_PRIMITIVE(world, iptr, EcsIPtr);
    ECS_PRIMITIVE(world, f32, EcsF32);
    ECS_PRIMITIVE(world, f64, EcsF64);
    ECS_PRIMITIVE(world, string, EcsString);
    ECS_PRIMITIVE(world, entity, EcsEntity);

    #undef ECS_PRIMITIVE
}

ecs_entity_t ecs_array_init(
    ecs_world_t *world,
    const ecs_array_desc_t *desc)
{
    ecs_entity_t t = ecs_entity_init(world, &desc->entity);
    if (!t) {
        return 0;
    }

    ecs_set(world, t, EcsArray, {
        .type = desc->type,
        .count = desc->count
    });

    return t;
}

ecs_entity_t ecs_vector_init(
    ecs_world_t *world,
    const ecs_vector_desc_t *desc)
{
    ecs_entity_t t = ecs_entity_init(world, &desc->entity);
    if (!t) {
        return 0;
    }

    ecs_set(world, t, EcsVector, {
        .type = desc->type
    });

    return t;
}

ecs_entity_t ecs_struct_init(
    ecs_world_t *world,
    const ecs_struct_desc_t *desc)
{
    ecs_entity_t t = ecs_entity_init(world, &desc->entity);
    if (!t) {
        return 0;
    }

    ecs_entity_t old_scope = ecs_set_scope(world, t);

    int i;
    for (i = 0; i < ECS_MEMBER_DESC_CACHE_SIZE; i ++) {
        const ecs_member_t *m_desc = &desc->members[i];
        if (!m_desc->type) {
            break;
        }

        if (!m_desc->name) {
            ecs_err("member %d of struct '%s' does not have a name", i, 
                ecs_get_name(world, t));
            ecs_delete(world, t);
            return 0;
        }

        ecs_entity_t m = ecs_entity_init(world, &(ecs_entity_desc_t) {
            .name = m_desc->name
        });

        ecs_set(world, m, EcsMember, {
            .type = m_desc->type, 
            .count = m_desc->count
        });
    }

    ecs_set_scope(world, old_scope);

    if (i == 0) {
        ecs_err("struct '%s' has no members", ecs_get_name(world, t));
        ecs_delete(world, t);
        return 0;
    }

    return t;
}

#endif