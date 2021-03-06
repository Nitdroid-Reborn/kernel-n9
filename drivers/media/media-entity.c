/*
 * Media entity
 *
 * Copyright (C) 2010 Nokia Corporation
 *
 * Contacts: Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 *	     Sakari Ailus <sakari.ailus@maxwell.research.nokia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <media/media-entity.h>
#include <media/media-device.h>

/**
 * media_entity_init - Initialize a media entity
 *
 * @num_pads: Total number of input and output pads.
 * @extra_links: Initial estimate of the number of extra links.
 * @pads: Array of 'num_pads' pads.
 *
 * The total number of pads is an intrinsic property of entities known by the
 * entity driver, while the total number of links depends on hardware design
 * and is an extrinsic property unknown to the entity driver. However, in most
 * use cases the entity driver can guess the number of links which can safely
 * be assumed to be equal to or larger than the number of pads.
 *
 * For those reasons the links array can be preallocated based on the entity
 * driver guess and will be reallocated later if extra links need to be
 * created.
 *
 * This function allocates a links array with enough space to hold at least
 * 'num_pads' + 'extra_links' elements. The media_entity::max_links field will
 * be set to the number of allocated elements.
 *
 * The pads array is managed by the entity driver and passed to
 * media_entity_init() where its pointer will be stored in the entity structure.
 */
int
media_entity_init(struct media_entity *entity, u16 num_pads,
		  struct media_pad *pads, u16 extra_links)
{
	struct media_link *links;
	unsigned int max_links = num_pads + extra_links;
	unsigned int i;

	links = kzalloc(max_links * sizeof(links[0]), GFP_KERNEL);
	if (links == NULL)
		return -ENOMEM;

	entity->group_id = 0;
	entity->max_links = max_links;
	entity->num_links = 0;
	entity->num_backlinks = 0;
	entity->num_pads = num_pads;
	entity->pads = pads;
	entity->links = links;

	for (i = 0; i < num_pads; i++) {
		pads[i].entity = entity;
		pads[i].index = i;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(media_entity_init);

void
media_entity_cleanup(struct media_entity *entity)
{
	kfree(entity->links);
}
EXPORT_SYMBOL_GPL(media_entity_cleanup);

/* -----------------------------------------------------------------------------
 * Graph traversal
 */

static struct media_entity *
media_entity_other(struct media_entity *entity, struct media_link *link)
{
	if (link->source->entity == entity)
		return link->sink->entity;
	else
		return link->source->entity;
}

/* push an entity to traversal stack */
static void stack_push(struct media_entity_graph *graph,
		       struct media_entity *entity)
{
	if (graph->top == MEDIA_ENTITY_ENUM_MAX_DEPTH - 1) {
		WARN_ON(1);
		return;
	}
	graph->top++;
	graph->stack[graph->top].link = 0;
	graph->stack[graph->top].entity = entity;
}

static struct media_entity *stack_pop(struct media_entity_graph *graph)
{
	struct media_entity *entity;

	entity = graph->stack[graph->top].entity;
	graph->top--;

	return entity;
}

#define stack_peek(en)	((en)->stack[(en)->top - 1].entity)
#define link_top(en)	((en)->stack[(en)->top].link)
#define stack_top(en)	((en)->stack[(en)->top].entity)

/**
 * media_entity_graph_walk_start - Start walking the media graph at a given entity
 * @graph: Media graph structure that will be used to walk the graph
 * @entity: Starting entity
 *
 * This function initializes the graph traversal structure to walk the entities
 * graph starting at the given entity. The traversal structure must not be
 * modified by the caller during graph traversal. When done the structure can
 * safely be freed.
 */
void media_entity_graph_walk_start(struct media_entity_graph *graph,
				   struct media_entity *entity)
{
	graph->top = 0;
	graph->stack[graph->top].entity = NULL;
	stack_push(graph, entity);
}
EXPORT_SYMBOL_GPL(media_entity_graph_walk_start);

/**
 * media_entity_graph_walk_next - Get the next entity in the graph
 * @graph: Media graph structure
 *
 * Perform a depth-first traversal of the given media entities graph.
 *
 * The graph structure must have been previously initialized with a call to
 * media_entity_graph_walk_start().
 *
 * Return the next entity in the graph or NULL if the whole graph have been
 * traversed.
 */
struct media_entity *
media_entity_graph_walk_next(struct media_entity_graph *graph)
{
	if (stack_top(graph) == NULL)
		return NULL;

	/*
	 * Depth first search. Push entity to stack and continue from
	 * top of the stack until no more entities on the level can be
	 * found.
	 */
	while (link_top(graph) < stack_top(graph)->num_links) {
		struct media_entity *entity = stack_top(graph);
		struct media_link *link = &entity->links[link_top(graph)];
		struct media_entity *next;

		/* The link is not enabled so we do not follow. */
		if (!(link->flags & MEDIA_LINK_FLAG_ENABLED)) {
			link_top(graph)++;
			continue;
		}

		/* Get the entity in the other end of the link . */
		next = media_entity_other(entity, link);

		/* Was it the entity we came here from? */
		if (next == stack_peek(graph)) {
			link_top(graph)++;
			continue;
		}

		/* Push the new entity to stack and start over. */
		link_top(graph)++;
		stack_push(graph, next);
	}

	return stack_pop(graph);
}
EXPORT_SYMBOL_GPL(media_entity_graph_walk_next);

/* -----------------------------------------------------------------------------
 * Pipeline management
 */

/**
 * media_entity_pipeline_start - Mark a pipeline as streaming
 * @entity: Starting entity
 * @pipe: Media pipeline to be assigned to all entities in the pipeline.
 *
 * Mark all entities connected to a given entity through enabled links, either
 * directly or indirectly, as streaming. The given pipeline object is assigned to
 * every entity in the pipeline and stored in the media_entity pipe field.
 *
 * Calls to this function can be nested, in which case the same number of
 * media_entity_pipeline_stop() calls will be required to stop streaming. The
 * pipeline pointer must be identical for all nested calls to
 * media_entity_pipeline_start().
 */
void media_entity_pipeline_start(struct media_entity *entity,
				 struct media_pipeline *pipe)
{
	struct media_device *mdev = entity->parent;
	struct media_entity_graph graph;

	mutex_lock(&mdev->graph_mutex);

	media_entity_graph_walk_start(&graph, entity);

	while ((entity = media_entity_graph_walk_next(&graph))) {
		entity->stream_count++;
		WARN_ON(entity->pipe && entity->pipe != pipe);
		entity->pipe = pipe;
	}

	mutex_unlock(&mdev->graph_mutex);
}
EXPORT_SYMBOL_GPL(media_entity_pipeline_start);

/**
 * media_entity_pipeline_stop - Mark a pipeline as not streaming
 * @entity: Starting entity
 *
 * Mark all entities connected to a given entity through enabled links, either
 * directly or indirectly, as not streaming. The media_entity pipe field is
 * reset to NULL.
 *
 * If multiple calls to media_entity_pipeline_start() have been made, the same
 * number of calls to this function are required to mark the pipeline as not
 * streaming.
 */
void media_entity_pipeline_stop(struct media_entity *entity)
{
	struct media_device *mdev = entity->parent;
	struct media_entity_graph graph;

	mutex_lock(&mdev->graph_mutex);

	media_entity_graph_walk_start(&graph, entity);

	while ((entity = media_entity_graph_walk_next(&graph))) {
		entity->stream_count--;
		if (entity->stream_count == 0)
			entity->pipe = NULL;
	}

	mutex_unlock(&mdev->graph_mutex);
}
EXPORT_SYMBOL_GPL(media_entity_pipeline_stop);

/* -----------------------------------------------------------------------------
 * Power state handling
 */

/*
 * Return power count of nodes directly or indirectly connected to
 * a given entity.
 */
static int media_entity_count_node(struct media_entity *entity)
{
	struct media_entity_graph graph;
	int use = 0;

	media_entity_graph_walk_start(&graph, entity);

	while ((entity = media_entity_graph_walk_next(&graph))) {
		if (media_entity_type(entity) == MEDIA_ENTITY_TYPE_DEVNODE)
			use += entity->use_count;
	}

	return use;
}

/* Apply use count to an entity. */
static void media_entity_use_apply_one(struct media_entity *entity, int change)
{
	entity->use_count += change;
	WARN_ON(entity->use_count < 0);
}

/*
 * Apply use count change to an entity and change power state based on
 * new use count.
 */
static int media_entity_power_apply_one(struct media_entity *entity, int change)
{
	int ret;

	if (entity->use_count == 0 && change > 0 &&
	    entity->ops && entity->ops->set_power) {
		ret = entity->ops->set_power(entity, 1);
		if (ret)
			return ret;
	}

	media_entity_use_apply_one(entity, change);

	if (entity->use_count == 0 && change < 0 &&
	    entity->ops && entity->ops->set_power)
		entity->ops->set_power(entity, 0);

	return 0;
}

/*
 * Apply power change to all connected entities. This ignores the
 * nodes.
 */
static int media_entity_power_apply(struct media_entity *entity, int change)
{
	struct media_entity_graph graph;
	struct media_entity *first = entity;
	int ret = 0;

	if (!change)
		return 0;

	media_entity_graph_walk_start(&graph, entity);

	while (!ret && (entity = media_entity_graph_walk_next(&graph)))
		if (media_entity_type(entity) != MEDIA_ENTITY_TYPE_DEVNODE)
			ret = media_entity_power_apply_one(entity, change);

	if (!ret)
		return 0;

	media_entity_graph_walk_start(&graph, first);

	while ((first = media_entity_graph_walk_next(&graph))
	       && first != entity)
		if (media_entity_type(first) != MEDIA_ENTITY_TYPE_DEVNODE)
			media_entity_power_apply_one(first, -change);

	return ret;
}

/* Apply the power state changes when connecting two entities. */
static int media_entity_power_connect(struct media_entity *one,
				      struct media_entity *theother)
{
	int power_one = media_entity_count_node(one);
	int power_theother = media_entity_count_node(theother);
	int ret = 0;

	ret = media_entity_power_apply(one, power_theother);
	if (ret < 0)
		return ret;

	return media_entity_power_apply(theother, power_one);
}

static void media_entity_power_disconnect(struct media_entity *one,
					  struct media_entity *theother)
{
	int power_one = media_entity_count_node(one);
	int power_theother = media_entity_count_node(theother);

	/* Powering off entities is assumed to never fail. */
	media_entity_power_apply(one, -power_theother);
	media_entity_power_apply(theother, -power_one);
}

/*
 * Apply use count change to graph and change power state of entities
 * accordingly.
 */
static int media_entity_node_power_change(struct media_entity *entity,
					  int change)
{
	/* Apply use count to node. */
	media_entity_use_apply_one(entity, change);

	/* Apply power change to connected non-nodes. */
	return media_entity_power_apply(entity, change);
}

/*
 * Node entity use changes are reflected on power state of all
 * connected (directly or indirectly) entities whereas non-node entity
 * use count changes are limited to that very entity.
 */
static int media_entity_use_change(struct media_entity *entity, int change)
{
	if (media_entity_type(entity) == MEDIA_ENTITY_TYPE_DEVNODE)
		return media_entity_node_power_change(entity, change);
	else
		return media_entity_power_apply_one(entity, change);
}

static struct media_entity *__media_entity_get(struct media_entity *entity)
{
	if (media_entity_use_change(entity, 1))
		return NULL;

	return entity;
}

static void __media_entity_put(struct media_entity *entity)
{
	media_entity_use_change(entity, -1);
}

/* user open()s media entity */
struct media_entity *media_entity_get(struct media_entity *entity)
{
	struct media_entity *e;

	if (entity == NULL)
		return NULL;

	if (entity->parent->dev &&
	    !try_module_get(entity->parent->dev->driver->owner))
		return NULL;

	mutex_lock(&entity->parent->graph_mutex);
	e = __media_entity_get(entity);
	mutex_unlock(&entity->parent->graph_mutex);

	if (e == NULL && entity->parent->dev)
		module_put(entity->parent->dev->driver->owner);

	return e;
}
EXPORT_SYMBOL_GPL(media_entity_get);

/* user release()s media entity */
void media_entity_put(struct media_entity *entity)
{
	if (entity == NULL)
		return;

	mutex_lock(&entity->parent->graph_mutex);
	__media_entity_put(entity);
	mutex_unlock(&entity->parent->graph_mutex);

	if (entity->parent->dev)
		module_put(entity->parent->dev->driver->owner);
}
EXPORT_SYMBOL_GPL(media_entity_put);

/* -----------------------------------------------------------------------------
 * Links management
 */

static struct media_link *media_entity_add_link(struct media_entity *entity)
{
	if (entity->num_links >= entity->max_links) {
		struct media_link *links = entity->links;
		unsigned int max_links = entity->max_links + 2;
		unsigned int i;

		links = krealloc(links, max_links * sizeof(*links), GFP_KERNEL);
		if (links == NULL)
			return NULL;

		for (i = 0; i < entity->num_links; i++)
			links[i].reverse->reverse = &links[i];

		entity->max_links = max_links;
		entity->links = links;
	}

	return &entity->links[entity->num_links++];
}

int
media_entity_create_link(struct media_entity *source, u16 source_pad,
			 struct media_entity *sink, u16 sink_pad, u32 flags)
{
	struct media_link *link;
	struct media_link *backlink;

	BUG_ON(source == NULL || sink == NULL);
	BUG_ON(source_pad >= source->num_pads);
	BUG_ON(sink_pad >= sink->num_pads);

	link = media_entity_add_link(source);
	if (link == NULL)
		return -ENOMEM;

	link->source = &source->pads[source_pad];
	link->sink = &sink->pads[sink_pad];
	link->flags = flags;

	/* Create the backlink. Backlinks are used to help graph traversal and
	 * are not reported to userspace.
	 */
	backlink = media_entity_add_link(sink);
	if (backlink == NULL) {
		source->num_links--;
		return -ENOMEM;
	}

	backlink->source = &source->pads[source_pad];
	backlink->sink = &sink->pads[sink_pad];
	backlink->flags = flags;

	link->reverse = backlink;
	backlink->reverse = link;

	sink->num_backlinks++;

	return 0;
}
EXPORT_SYMBOL_GPL(media_entity_create_link);

static int __media_entity_setup_link_notify(struct media_link *link, u32 flags)
{
	const u32 mask = MEDIA_LINK_FLAG_ENABLED;
	int ret;

	/* Notify both entities. */
	ret = media_entity_call(link->source->entity, link_setup,
				link->source, link->sink, flags);
	if (ret < 0 && ret != -ENOIOCTLCMD)
		return ret;

	ret = media_entity_call(link->sink->entity, link_setup,
				link->sink, link->source, flags);
	if (ret < 0 && ret != -ENOIOCTLCMD) {
		media_entity_call(link->source->entity, link_setup,
				  link->source, link->sink, link->flags);
		return ret;
	}

	link->flags = (link->flags & ~mask) | (flags & mask);
	link->reverse->flags = link->flags;

	return 0;
}

/**
 * __media_entity_setup_link - Configure a media link
 * @link: The link being configured
 * @flags: Link configuration flags
 *
 * The bulk of link setup is handled by the two entities connected through the
 * link. This function notifies both entities of the link configuration change.
 *
 * If the link is immutable or if the current and new configuration are
 * identical, return immediately.
 *
 * The user is expected to hold link->source->parent->mutex. If not,
 * media_entity_setup_link() should be used instead.
 */
int __media_entity_setup_link(struct media_link *link, u32 flags)
{
	struct media_entity *source, *sink;
	int ret = -EBUSY;

	if (link == NULL)
		return -EINVAL;

	if (link->flags & MEDIA_LINK_FLAG_IMMUTABLE)
		return link->flags == flags ? 0 : -EINVAL;

	if (link->flags == flags)
		return 0;

	if (!(link->flags & MEDIA_LINK_FLAG_DYNAMIC) &&
	    (link->source->entity->stream_count ||
	     link->sink->entity->stream_count))
		return -EBUSY;

	source = __media_entity_get(link->source->entity);
	if (!source)
		return ret;

	sink = __media_entity_get(link->sink->entity);
	if (!sink)
		goto err___media_entity_get;

	if (flags & MEDIA_LINK_FLAG_ENABLED) {
		ret = media_entity_power_connect(source, sink);
		if (ret < 0)
			goto err_media_entity_power_connect;
	}

	ret = __media_entity_setup_link_notify(link, flags);
	if (ret < 0)
		goto err___media_entity_setup_link_notify;

	if (!(flags & MEDIA_LINK_FLAG_ENABLED))
		media_entity_power_disconnect(source, sink);

	__media_entity_put(sink);
	__media_entity_put(source);

	return 0;

err___media_entity_setup_link_notify:
	if (flags & MEDIA_LINK_FLAG_ENABLED)
		media_entity_power_disconnect(source, sink);
err_media_entity_power_connect:
	__media_entity_put(sink);
err___media_entity_get:
	__media_entity_put(source);

	return ret;
}

int media_entity_setup_link(struct media_link *link, u32 flags)
{
	int ret;

	mutex_lock(&link->source->entity->parent->graph_mutex);
	ret = __media_entity_setup_link(link, flags);
	mutex_unlock(&link->source->entity->parent->graph_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(media_entity_setup_link);

/**
 * media_entity_find_link - Find a link between two pads
 * @source: Source pad
 * @sink: Sink pad
 *
 * Return a pointer to the link between the two entities. If no such link
 * exists, return NULL.
 */
struct media_link *
media_entity_find_link(struct media_pad *source, struct media_pad *sink)
{
	struct media_link *link;
	unsigned int i;

	for (i = 0; i < source->entity->num_links; ++i) {
		link = &source->entity->links[i];

		if (link->source->entity == source->entity &&
		    link->source->index == source->index &&
		    link->sink->entity == sink->entity &&
		    link->sink->index == sink->index)
			return link;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(media_entity_find_link);

/**
 * media_entity_remote_source - Find the source pad at the remote end of a link
 * @pad: Sink pad at the local end of the link
 *
 * Search for a remote source pad connected to the given sink pad by iterating
 * over all links originating or terminating at that pad until an enabled link
 * is found.
 *
 * Return a pointer to the pad at the remote end of the first found enabled
 * link, or NULL if no enabled link has been found.
 */
struct media_pad *media_entity_remote_source(struct media_pad *pad)
{
	unsigned int i;

	for (i = 0; i < pad->entity->num_links; i++) {
		struct media_link *link = &pad->entity->links[i];

		if (!(link->flags & MEDIA_LINK_FLAG_ENABLED))
			continue;

		if (link->source == pad)
			return link->sink;

		if (link->sink == pad)
			return link->source;
	}

	return NULL;

}
EXPORT_SYMBOL_GPL(media_entity_remote_source);
