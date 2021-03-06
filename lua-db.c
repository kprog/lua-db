#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <string.h>
#include <assert.h>

#define DATA 1
#define INDEX 2
#define THREAD_MAX 32

static lua_State * g_dbL = NULL;
static int g_threadcount = 1;
static __thread int t_threadinit = 0;
static __thread int t_threadindex = 0;

static void
_init(lua_State *L) {
	const char * loader = luaL_checkstring(L,1);
	lua_State * dbL = luaL_newstate();
	lua_gc(dbL, LUA_GCSTOP, 0);
	luaL_openlibs(dbL);  
	lua_gc(dbL, LUA_GCRESTART, 0);

	int top = lua_gettop(dbL);
	int err = luaL_dofile(dbL, loader);
	if (err) {
		luaL_error(L, "%s" , lua_tostring(dbL,-1));
	}
	lua_settop(dbL, top+1);
	lua_insert(dbL, DATA);

	if (lua_type(dbL, DATA) != LUA_TTABLE) {
		lua_close(dbL);
		luaL_error(L, "%s need return a table", loader);
	}

	lua_newtable(dbL);
	lua_rawgeti(dbL, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
	lua_setfield(dbL, -2, "__index");
	lua_setmetatable(dbL, DATA);

	const char * posloader = luaL_checkstring(L,2);

	err = luaL_loadstring(dbL, posloader);
	if (err) {
		lua_close(dbL);
		luaL_error(L, "load posload error");
	}

	lua_pushvalue(dbL,DATA);
	lua_setupvalue(dbL, -2, 1);  // set DATA to _ENV of posloader 

	err = lua_pcall(dbL, 0, 1, 0);

	if (err) {
		lua_close(dbL);
		luaL_error(L, "posload : %s" , lua_tostring(dbL,-1));
	}

	lua_insert(dbL, INDEX);

	if (lua_type(dbL, INDEX) != LUA_TTABLE) {
		lua_close(dbL);
		luaL_error(L, "posload need return a table");
	}

	lua_pushstring(dbL, loader);
	lua_setfield(dbL, LUA_REGISTRYINDEX , "loader");

	lua_settop(dbL, 2);

	lua_checkstack(dbL , THREAD_MAX + 2);
	int i;
	for (i=0;i < THREAD_MAX ; i++) {
		lua_State * thread = lua_newthread(dbL);
		lua_checkstack(thread , LUA_MINSTACK);
		lua_pushvalue(dbL , DATA);
		lua_pushvalue(dbL , INDEX);
		lua_xmove(dbL , thread , 2);
	}

	lua_remove(dbL , 1);
	lua_remove(dbL , 1);

	lua_gc(dbL, LUA_GCCOLLECT , 0);
	lua_gc(dbL, LUA_GCSTOP, 0);


	g_dbL = dbL;
}


static int
db_open(lua_State *L) {
	int threadinit = __sync_val_compare_and_swap (&t_threadinit, 0, 1);
	if (threadinit == 0) {
		t_threadindex = __sync_fetch_and_add (&g_threadcount, 1);

		if (t_threadindex == 1) {
			// init
			_init(L);
		}

		__sync_synchronize();

		if (t_threadindex > THREAD_MAX) {
			return luaL_error(L, "The number of threads is too a lot");
		}
		
	} else {
		while (t_threadindex == 0) {
			__sync_synchronize();
		}

		if (g_dbL == NULL) {
			return luaL_error(L, "Global database init failed");
		}

		lua_State * dbL = lua_tothread(g_dbL, t_threadindex);
		assert(dbL);

		const char * loader = luaL_checkstring(L,1);

		lua_getfield(dbL, LUA_REGISTRYINDEX , "loader");
		if (lua_type(dbL,-1) != LUA_TSTRING) {
			lua_settop(dbL, 2);
			return luaL_error(L, "loader is not exist in db state");
		}
		const char * db_loader = lua_tostring(dbL,-1);
		if (strcmp(loader,db_loader) != 0) {
			lua_settop(dbL, 2);
			return luaL_error(L, "db state loader is not the same with (%s)", loader);
		}
		lua_settop(dbL, 2);
	}
	int mem = lua_gc(g_dbL, LUA_GCCOUNT , 0);
	lua_pushinteger(L, mem);
	lua_pushinteger(L, t_threadindex);
	return 2;
}

static int
db_get(lua_State *L) {
	lua_State * dbL = lua_tothread(g_dbL, t_threadindex);
	assert(dbL);

	size_t sz = 0;
	const char * key = luaL_checklstring(L,1,&sz);

	lua_pushlstring(dbL,key,sz);
	lua_pushvalue(dbL,-1);
	lua_rawget(dbL,INDEX);

	int type = lua_type(dbL,-1);
	switch(type) {
	case LUA_TNIL:
		lua_pushnil(L);
		break;
	case LUA_TNUMBER: {
		lua_Number v = lua_tonumber(dbL,-1);
		lua_pushnumber(L,v);
		break;
	}
	case LUA_TSTRING: {
		const char *v = lua_tolstring(dbL,-1,&sz);
		lua_pushlstring(L,v,sz);
		break;
	}
	case LUA_TBOOLEAN: {
		int v = lua_toboolean(dbL,-1);
		lua_pushboolean(L,v);
		break;
	}
	case LUA_TTABLE : {
		void * keyname = (void*)lua_tostring(dbL, -2);
		assert(keyname);
		lua_pushlightuserdata(L, keyname);
		lua_pushliteral(L, "table");
		lua_pop(dbL,2);
		return 2;
	}
	case LUA_TFUNCTION: {
		void * keyname = (void*)lua_tostring(dbL, -2);
		lua_pushlightuserdata(L, keyname);
		lua_pushliteral(L, "function");
		lua_pop(dbL,2);
		return 2;
	}
	default:
		lua_pop(dbL,2);
		luaL_error(L,"unsupported type: %s",lua_typename(dbL, type));
		break;
	}

	lua_pop(dbL,2);
	return 1;
}

static int
db_expend(lua_State *L) {
	lua_State * dbL = lua_tothread(g_dbL, t_threadindex);
	int type = lua_type(L,1);
	const char * key = NULL;
	switch (type) {
	case LUA_TLIGHTUSERDATA :
		key = (const char*)lua_touserdata(L,1);
		lua_pushstring(dbL, key);
		break;
	case LUA_TSTRING : {
		size_t sz = 0;
		key = lua_tolstring(L,1,&sz);
		lua_pushlstring(dbL,key,sz);
		break;
	}
	default:
		return luaL_error(L, "expand need a string");
	}
	lua_rawget(dbL, INDEX);
	if (lua_isfunction(dbL, -1)) {
		lua_rawget(dbL, INDEX);
		if (!lua_isstring(dbL, -1)) {
			lua_pop(dbL, 1);
			return luaL_error(L, "expand an invalid function %s", key);
		}
		size_t sz = 0;
		const char * code = lua_tolstring(dbL,-1,&sz);
		lua_pushlstring(L, code, sz);
		lua_pop(dbL, 1);
		return 1;
	}

	if (!lua_istable(dbL, -1)) {
		return luaL_error(L, "%s is not a table or function", key);
	}

	int len = lua_rawlen(dbL, -1);
	lua_checkstack(L, 2 + len);
	int i;
	for (i=0;i<len;i++) {
		lua_rawgeti(dbL, -1, i+1);
		size_t sz = 0;
		const char * v = lua_tolstring(dbL, -1 , &sz);
		if (v == NULL) {
			return luaL_error(L, "%s is an invalid table", key);
		}
		lua_pushlstring(L, v , sz);
		lua_pop(dbL,1);
	}
	lua_settop(dbL,2);

	return len;
}

int
luaopen_database_c(lua_State *L) {
	luaL_Reg l[] = {
		{ "open" , db_open },
		{ "get" , db_get },
		{ "expend" , db_expend },
		{ NULL, NULL },
	};
	luaL_checkversion(L);
	luaL_newlib(L,l);
	return 1;
}
