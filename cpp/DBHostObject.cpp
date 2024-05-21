#include "DBHostObject.h"
#include "PreparedStatementHostObject.h"
#include "bridge.h"
#include "macros.h"
#include "sqlbatchexecutor.h"
#include "utils.h"
#include <iostream>

namespace opsqlite {

namespace jsi = facebook::jsi;
namespace react = facebook::react;

DBHostObject::DBHostObject(jsi::Runtime &rt, std::string &base_path,
                           std::shared_ptr<react::CallInvoker> jsCallInvoker,
                           std::shared_ptr<ThreadPool> thread_pool,
                           std::string &db_name, std::string &path,
                           std::string &crsqlite_path,
                           std::string &encryption_key)
    : base_path(base_path), jsCallInvoker(jsCallInvoker),
      thread_pool(thread_pool), db_name(db_name) {

#ifdef OP_SQLITE_USE_SQLCIPHER
  BridgeResult result =
      opsqlite_open(db_name, path, crsqlite_path, encryption_key);
#else
  BridgeResult result = opsqlite_open(db_name, path, crsqlite_path);
#endif

  if (result.type == SQLiteError) {
    throw std::runtime_error(result.message);
  }

  auto attach = HOSTFN("attach", 4) {
    if (count < 3) {
      throw jsi::JSError(rt,
                         "[op-sqlite][attach] Incorrect number of arguments");
    }
    if (!args[0].isString() || !args[1].isString() || !args[2].isString()) {
      throw jsi::JSError(
          rt, "dbName, databaseToAttach and alias must be a strings");
      return {};
    }

    std::string tempDocPath = std::string(base_path);
    if (count > 3 && !args[3].isUndefined() && !args[3].isNull()) {
      if (!args[3].isString()) {
        throw std::runtime_error(
            "[op-sqlite][attach] database location must be a string");
      }

      tempDocPath = tempDocPath + "/" + args[3].asString(rt).utf8(rt);
    }

    std::string dbName = args[0].asString(rt).utf8(rt);
    std::string databaseToAttach = args[1].asString(rt).utf8(rt);
    std::string alias = args[2].asString(rt).utf8(rt);
    BridgeResult result =
        opsqlite_attach(dbName, tempDocPath, databaseToAttach, alias);

    if (result.type == SQLiteError) {
      throw std::runtime_error(result.message);
    }

    return {};
  });

  auto detach = HOSTFN("detach", 2) {
    if (count < 2) {
      throw std::runtime_error(
          "[op-sqlite][detach] Incorrect number of arguments");
    }
    if (!args[0].isString() || !args[1].isString()) {
      throw std::runtime_error(
          "dbName, databaseToAttach and alias must be a strings");
      return {};
    }

    std::string dbName = args[0].asString(rt).utf8(rt);
    std::string alias = args[1].asString(rt).utf8(rt);
    BridgeResult result = opsqlite_detach(dbName, alias);

    if (result.type == SQLiteError) {
      throw jsi::JSError(rt, result.message.c_str());
    }

    return {};
  });

  auto close = HOSTFN("close", 0) {
    BridgeResult result = opsqlite_close(db_name);

    if (result.type == SQLiteError) {
      throw jsi::JSError(rt, result.message.c_str());
    }

    return {};
  });

  auto remove = HOSTFN("delete", 1) {

    std::string location = std::string(base_path);

    if (count == 1 && !args[0].isUndefined() && !args[0].isNull()) {
      if (!args[1].isString()) {
        throw std::runtime_error(
            "[op-sqlite][open] database location must be a string");
      }

      location = location + "/" + args[1].asString(rt).utf8(rt);
    }

    BridgeResult result = opsqlite_remove(db_name, location);

    if (result.type == SQLiteError) {
      throw std::runtime_error(result.message);
    }

    return {};
  });

  auto execute = HOSTFN("execute", 2) {
    const std::string query = args[0].asString(rt).utf8(rt);
    std::vector<JSVariant> params;

    if (count == 2) {
      const jsi::Value &originalParams = args[1];
      params = toVariantVec(rt, originalParams);
    }

    std::vector<DumbHostObject> results;
    std::shared_ptr<std::vector<SmartHostObject>> metadata =
        std::make_shared<std::vector<SmartHostObject>>();

    auto status = opsqlite_execute(db_name, query, &params, &results, metadata);

    if (status.type == SQLiteError) {
      throw std::runtime_error(status.message);
    }

    auto jsiResult = createResult(rt, status, &results, metadata);
    return jsiResult;
  });

  auto execute_raw_async = HOSTFN("executeRawAsync", 2) {
    const std::string query = args[0].asString(rt).utf8(rt);
    std::vector<JSVariant> params;

    if (count == 2) {
      const jsi::Value &originalParams = args[1];
      params = toVariantVec(rt, originalParams);
    }

    auto promiseCtr = rt.global().getPropertyAsFunction(rt, "Promise");
    auto promise = promiseCtr.callAsConstructor(rt, HOSTFN("executor", 2) {
      auto resolve = std::make_shared<jsi::Value>(rt, args[0]);
      auto reject = std::make_shared<jsi::Value>(rt, args[1]);

      auto task = [&rt, db_name, query, params = std::move(params), resolve,
                   reject, invoker = this->jsCallInvoker]() {
        try {
          std::vector<std::vector<JSVariant>> results;

          auto status = opsqlite_execute_raw(db_name, query, &params, &results);
          //
          //            if (invalidated) {
          //              return;
          //            }

          invoker->invokeAsync([&rt, results = std::move(results),
                                status = std::move(status), resolve, reject] {
            if (status.type == SQLiteOk) {
              auto jsiResult = create_raw_result(rt, status, &results);
              resolve->asObject(rt).asFunction(rt).call(rt,
                                                        std::move(jsiResult));
            } else {
              auto errorCtr = rt.global().getPropertyAsFunction(rt, "Error");
              auto error = errorCtr.callAsConstructor(
                  rt, jsi::String::createFromUtf8(rt, status.message));
              reject->asObject(rt).asFunction(rt).call(rt, error);
            }
          });

        } catch (std::exception &exc) {
          invoker->invokeAsync([&rt, exc = std::move(exc), reject] {
            auto errorCtr = rt.global().getPropertyAsFunction(rt, "Error");
            auto error = errorCtr.callAsConstructor(
                rt, jsi::String::createFromAscii(rt, exc.what()));
            reject->asObject(rt).asFunction(rt).call(rt, error);
          });
        }
      };

      thread_pool->queueWork(task);

      return {};
    }));

    return promise;
  });

  auto execute_async = HOSTFN("executeAsync", 2) {

    const std::string query = args[0].asString(rt).utf8(rt);
    std::vector<JSVariant> params;

    if (count == 2) {
      const jsi::Value &originalParams = args[1];
      params = toVariantVec(rt, originalParams);
    }

    auto promiseCtr = rt.global().getPropertyAsFunction(rt, "Promise");
    auto promise = promiseCtr.callAsConstructor(rt, HOSTFN("executor", 2) {
      auto resolve = std::make_shared<jsi::Value>(rt, args[0]);
      auto reject = std::make_shared<jsi::Value>(rt, args[1]);

      auto task = [&rt, &db_name, query, params = std::move(params), resolve,
                   reject, invoker = this->jsCallInvoker]() {
        try {
          std::vector<DumbHostObject> results;
          std::shared_ptr<std::vector<SmartHostObject>> metadata =
              std::make_shared<std::vector<SmartHostObject>>();

          auto status =
              opsqlite_execute(db_name, query, &params, &results, metadata);

          //            if (invalidated) {
          //              return;
          //            }

          invoker->invokeAsync(
              [&rt,
               results = std::make_shared<std::vector<DumbHostObject>>(results),
               metadata, status = std::move(status), resolve, reject] {
                if (status.type == SQLiteOk) {
                  auto jsiResult =
                      createResult(rt, status, results.get(), metadata);
                  resolve->asObject(rt).asFunction(rt).call(
                      rt, std::move(jsiResult));
                } else {
                  auto errorCtr =
                      rt.global().getPropertyAsFunction(rt, "Error");
                  auto error = errorCtr.callAsConstructor(
                      rt, jsi::String::createFromUtf8(rt, status.message));
                  reject->asObject(rt).asFunction(rt).call(rt, error);
                }
              });

        } catch (std::exception &exc) {
          invoker->invokeAsync([&rt, exc = std::move(exc), reject] {
            auto errorCtr = rt.global().getPropertyAsFunction(rt, "Error");
            auto error = errorCtr.callAsConstructor(
                rt, jsi::String::createFromAscii(rt, exc.what()));
            reject->asObject(rt).asFunction(rt).call(rt, error);
          });
        }
      };

      thread_pool->queueWork(task);

      return {};
      }));

    return promise;
  });

  auto execute_batch = HOSTFN("executeBatch", 1) {
    if (sizeof(args) < 1) {
      throw std::runtime_error(
          "[op-sqlite][executeBatch] - Incorrect parameter count");
    }

    const jsi::Value &params = args[0];
    if (params.isNull() || params.isUndefined()) {
      throw std::runtime_error("[op-sqlite][executeBatch] - An array of SQL "
                               "commands or parameters is needed");
    }
    const jsi::Array &batchParams = params.asObject(rt).asArray(rt);
    std::vector<BatchArguments> commands;
    toBatchArguments(rt, batchParams, &commands);

    auto batchResult = sqliteExecuteBatch(db_name, &commands);
    if (batchResult.type == SQLiteOk) {
      auto res = jsi::Object(rt);
      res.setProperty(rt, "rowsAffected", jsi::Value(batchResult.affectedRows));
      return std::move(res);
    } else {
      throw std::runtime_error(batchResult.message);
    }
  });

  auto execute_batch_async = HOSTFN("executeBatchAsync", 1) {
    if (sizeof(args) < 1) {
      throw std::runtime_error(
          "[op-sqlite][executeAsyncBatch] Incorrect parameter count");
      return {};
    }

    const jsi::Value &params = args[0];

    if (params.isNull() || params.isUndefined()) {
      throw std::runtime_error(
          "[op-sqlite][executeAsyncBatch] - An array of SQL "
          "commands or parameters is needed");
      return {};
    }

    const jsi::Array &batchParams = params.asObject(rt).asArray(rt);

    std::vector<BatchArguments> commands;
    toBatchArguments(rt, batchParams, &commands);

    auto promiseCtr = rt.global().getPropertyAsFunction(rt, "Promise");
    auto promise = promiseCtr.callAsConstructor(rt, HOSTFN("executor", 2) {
      auto resolve = std::make_shared<jsi::Value>(rt, args[0]);
      auto reject = std::make_shared<jsi::Value>(rt, args[1]);

      auto task = [&rt, &db_name, &jsCallInvoker,
                   commands =
                       std::make_shared<std::vector<BatchArguments>>(commands),
                   resolve, reject]() {
        try {
          auto batchResult = sqliteExecuteBatch(db_name, commands.get());
          jsCallInvoker->invokeAsync([&rt, batchResult = std::move(batchResult),
                                      resolve, reject] {
            if (batchResult.type == SQLiteOk) {
              auto res = jsi::Object(rt);
              res.setProperty(rt, "rowsAffected",
                              jsi::Value(batchResult.affectedRows));
              resolve->asObject(rt).asFunction(rt).call(rt, std::move(res));
            } else {
              auto errorCtr = rt.global().getPropertyAsFunction(rt, "Error");
              auto error = errorCtr.callAsConstructor(
                  rt, jsi::String::createFromUtf8(rt, batchResult.message));
              reject->asObject(rt).asFunction(rt).call(rt, error);
            }
          });
        } catch (std::exception &exc) {
          jsCallInvoker->invokeAsync(
              [&rt, reject, &exc] { throw jsi::JSError(rt, exc.what()); });
        }
      };
      thread_pool->queueWork(task);

      return {};
           }));

    return promise;
  });

  auto load_file = HOSTFN("loadFile", 1) {
    if (sizeof(args) < 1) {
      throw std::runtime_error(
          "[op-sqlite][loadFile] Incorrect parameter count");
      return {};
    }

    const std::string sqlFileName = args[0].asString(rt).utf8(rt);

    auto promiseCtr = rt.global().getPropertyAsFunction(rt, "Promise");
    auto promise = promiseCtr.callAsConstructor(rt, HOSTFN("executor", 2) {
      auto resolve = std::make_shared<jsi::Value>(rt, args[0]);
      auto reject = std::make_shared<jsi::Value>(rt, args[1]);

      auto task = [&rt, &db_name, &jsCallInvoker, sqlFileName, resolve,
                   reject]() {
        try {
          const auto importResult = importSQLFile(db_name, sqlFileName);

          jsCallInvoker->invokeAsync([&rt, result = std::move(importResult),
                                      resolve, reject] {
            if (result.type == SQLiteOk) {
              auto res = jsi::Object(rt);
              res.setProperty(rt, "rowsAffected",
                              jsi::Value(result.affectedRows));
              res.setProperty(rt, "commands", jsi::Value(result.commands));
              resolve->asObject(rt).asFunction(rt).call(rt, std::move(res));
            } else {
              auto errorCtr = rt.global().getPropertyAsFunction(rt, "Error");
              auto error = errorCtr.callAsConstructor(
                  rt, jsi::String::createFromUtf8(rt, result.message));
              reject->asObject(rt).asFunction(rt).call(rt, error);
            }
          });
        } catch (std::exception &exc) {
          jsCallInvoker->invokeAsync(
              [&rt, err = exc.what(), reject] { throw jsi::JSError(rt, err); });
        }
      };
      thread_pool->queueWork(task);
      return {};
           }));

    return promise;
  });

  //    auto update_hook = HOSTFN("updateHook", 2) {
  //      if (sizeof(args) < 2) {
  //        throw std::runtime_error("[op-sqlite][updateHook] Incorrect
  //        parameters: "
  //                                 "dbName and callback needed");
  //        return {};
  //      }
  //
  //      auto dbName = args[0].asString(rt).utf8(rt);
  //      auto callback = std::make_shared<jsi::Value>(rt, args[1]);
  //
  //      if (callback->isUndefined() || callback->isNull()) {
  //        opsqlite_deregister_update_hook(dbName);
  //        return {};
  //      }
  //
  //      updateHooks[dbName] = callback;
  //
  //      auto hook = [&rt, callback](std::string dbName, std::string tableName,
  //                                  std::string operation, int rowId) {
  //        std::vector<JSVariant> params;
  //        std::vector<DumbHostObject> results;
  //        std::shared_ptr<std::vector<SmartHostObject>> metadata =
  //            std::make_shared<std::vector<SmartHostObject>>();
  //
  //        if (operation != "DELETE") {
  //          std::string query = "SELECT * FROM " + tableName +
  //                              " where rowid = " + std::to_string(rowId) +
  //                              ";";
  //          opsqlite_execute(dbName, query, &params, &results, metadata);
  //        }
  //
  //        invoker->invokeAsync(
  //            [&rt,
  //             results =
  //             std::make_shared<std::vector<DumbHostObject>>(results),
  //             callback, tableName = std::move(tableName),
  //             operation = std::move(operation), &rowId] {
  //              auto res = jsi::Object(rt);
  //              res.setProperty(rt, "table",
  //                              jsi::String::createFromUtf8(rt, tableName));
  //              res.setProperty(rt, "operation",
  //                              jsi::String::createFromUtf8(rt, operation));
  //              res.setProperty(rt, "rowId", jsi::Value(rowId));
  //              if (results->size() != 0) {
  //                res.setProperty(
  //                    rt, "row",
  //                    jsi::Object::createFromHostObject(
  //                        rt,
  //                        std::make_shared<DumbHostObject>(results->at(0))));
  //              }
  //
  //              callback->asObject(rt).asFunction(rt).call(rt, res);
  //            });
  //      };
  //
  //      opsqlite_register_update_hook(dbName, std::move(hook));
  //
  //      return {};
  //    });
  //
  //    auto commit_hook = HOSTFN("commitHook", 2) {
  //      if (sizeof(args) < 2) {
  //        throw std::runtime_error("[op-sqlite][commitHook] Incorrect
  //        parameters: "
  //                                 "dbName and callback needed");
  //        return {};
  //      }
  //
  //      auto dbName = args[0].asString(rt).utf8(rt);
  //      auto callback = std::make_shared<jsi::Value>(rt, args[1]);
  //      if (callback->isUndefined() || callback->isNull()) {
  //        opsqlite_deregister_commit_hook(dbName);
  //        return {};
  //      }
  //      commitHooks[dbName] = callback;
  //
  //      auto hook = [&rt, callback](std::string dbName) {
  //        invoker->invokeAsync(
  //            [&rt, callback] {
  //            callback->asObject(rt).asFunction(rt).call(rt); });
  //      };
  //
  //      opsqlite_register_commit_hook(dbName, std::move(hook));
  //
  //      return {};
  //    });
  //
  //    auto rollback_hook = HOSTFN("rollbackHook", 2) {
  //      if (sizeof(args) < 2) {
  //        throw std::runtime_error(
  //            "[op-sqlite][rollbackHook] Incorrect parameters: "
  //            "dbName and callback needed");
  //        return {};
  //      }
  //
  //      auto dbName = args[0].asString(rt).utf8(rt);
  //      auto callback = std::make_shared<jsi::Value>(rt, args[1]);
  //
  //      if (callback->isUndefined() || callback->isNull()) {
  //        opsqlite_deregister_rollback_hook(dbName);
  //        return {};
  //      }
  //      rollbackHooks[dbName] = callback;
  //
  //      auto hook = [&rt, callback](std::string dbName) {
  //        invoker->invokeAsync(
  //            [&rt, callback] {
  //            callback->asObject(rt).asFunction(rt).call(rt); });
  //      };
  //
  //      opsqlite_register_rollback_hook(dbName, std::move(hook));
  //      return {};
  //    });

  auto prepare_statement = HOSTFN("prepareStatement", 1) {
    auto query = args[0].asString(rt).utf8(rt);

    sqlite3_stmt *statement = opsqlite_prepare_statement(db_name, query);

    auto preparedStatementHostObject =
        std::make_shared<PreparedStatementHostObject>(db_name, statement);

    return jsi::Object::createFromHostObject(rt, preparedStatementHostObject);
  });

  auto load_extension = HOSTFN("loadExtension", 1) {
    auto path = args[0].asString(rt).utf8(rt);
    std::string entry_point = "";
    if (count > 1 && args[1].isString()) {
      entry_point = args[1].asString(rt).utf8(rt);
    }

    auto result = opsqlite_load_extension(db_name, path, entry_point);
    if (result.type == SQLiteError) {
      throw std::runtime_error(result.message);
    }
    return {};
  });

  auto get_db_path = HOSTFN("getDbPath", 1) {
    std::string path = std::string(base_path);
    if (count == 1 && !args[0].isUndefined() && !args[0].isNull()) {
      if (!args[0].isString()) {
        throw std::runtime_error(
            "[op-sqlite][open] database location must be a string");
      }

      std::string lastPath = args[0].asString(rt).utf8(rt);

      if (lastPath == ":memory:") {
        path = ":memory:";
      } else if (lastPath.rfind("/", 0) == 0) {
        path = lastPath;
      } else {
        path = path + "/" + lastPath;
      }
    }

    auto result = opsqlite_get_db_path(db_name, path);
    return jsi::String::createFromUtf8(rt, result);
  });

  function_map["attach"] = std::move(attach);
  function_map["detach"] = std::move(detach);
  function_map["close"] = std::move(close);
  function_map["executeRawAsync"] = std::move(execute_raw_async);
  function_map["execute"] = std::move(execute);
  function_map["executeAsync"] = std::move(execute_async);
  function_map["delete"] = std::move(remove);
  function_map["executeBatch"] = std::move(execute_batch);
  function_map["executeBatchAsync"] = std::move(execute_batch_async);
  function_map["loadFile"] = std::move(load_file);
  //    function_map["updateHook"] = std::move(update_hook);
  //    function_map["commitHook"] = std::move(commit_hook);
  //    function_map["rollbackHook"] = std::move(rollback_hook);
  function_map["prepareStatement"] = std::move(prepare_statement);
  function_map["loadExtension"] = std::move(load_extension);
  function_map["getDbPath"] = std::move(get_db_path);
};

std::vector<jsi::PropNameID> DBHostObject::getPropertyNames(jsi::Runtime &rt) {
  std::vector<jsi::PropNameID> keys;

  return keys;
}

jsi::Value DBHostObject::get(jsi::Runtime &rt,
                             const jsi::PropNameID &propNameID) {

  auto name = propNameID.utf8(rt);
  if (name == "attach") {
    return jsi::Value(rt, function_map["attach"]);
  }
  if (name == "detach") {
    return jsi::Value(rt, function_map["detach"]);
  }
  if (name == "close") {
    return jsi::Value(rt, function_map["close"]);
  }
  if (name == "executeRawAsync") {
    return jsi::Value(rt, function_map["executeRawAsync"]);
  }
  if (name == "execute") {
    return jsi::Value(rt, function_map["execute"]);
  }
  if (name == "executeAsync") {
    return jsi::Value(rt, function_map["executeAsync"]);
  }
  if (name == "delete") {
    return jsi::Value(rt, function_map["delete"]);
  }
  if (name == "executeBatch") {
    return jsi::Value(rt, function_map["executeBatch"]);
  }
  if (name == "executeBatchAsync") {
    return jsi::Value(rt, function_map["executeBatchAsync"]);
  }
  if (name == "loadFile") {
    return jsi::Value(rt, function_map["loadFile"]);
  }
  //    if (name == "updateHook") {
  //      return jsi::Value(rt, function_map["updateHook"]);
  //    }
  //    if (name == "commitHook") {
  //      return jsi::Value(rt, function_map["commitHook"]);
  //    }
  //    if (name == "rollbackHook") {
  //      return jsi::Value(rt, function_map["rollbackHook"]);
  //    }
  if (name == "prepareStatement") {
    return jsi::Value(rt, function_map["prepareStatement"]);
  }
  if (name == "loadExtension") {
    return jsi::Value(rt, function_map["loadExtension"]);
  }
  if (name == "getDbPath") {
    return jsi::Value(rt, function_map["getDbPath"]);
  }

  return {};
}

// void DBHostObject::set(jsi::Runtime &rt, const jsi::PropNameID &name,
//                        const jsi::Value &value) {
//   throw std::runtime_error("You cannot write to this object!");
// }

} // namespace opsqlite