#include "core/global.h"
#include "core/datetime.h"
#include "core/makedir.h"
#include "core/config_parser.h"
#include "core/timer.h"
#include "core/threadsafequeue.h"
#include "dataio/sgf.h"
#include "dataio/trainingwrite.h"
#include "dataio/loadmodel.h"
#include "neuralnet/modelversion.h"
#include "search/asyncbot.h"
#include "program/setup.h"
#include "program/play.h"
#include "program/selfplaymanager.h"
#include "main.h"

#define TCLAP_NAMESTARTSTRING "-" //Use single dashes for all flags
#include <tclap/CmdLine.h>

#include <chrono>
#include <csignal>

using namespace std;

static std::atomic<bool> sigReceived(false);
static std::atomic<bool> shouldStop(false);
static void signalHandler(int signal)
{
  if(signal == SIGINT || signal == SIGTERM) {
    sigReceived.store(true);
    shouldStop.store(true);
  }
}

//-----------------------------------------------------------------------------------------


int MainCmds::selfplay(int argc, const char* const* argv) {
  Board::initHash();
  ScoreValue::initTables();
  Rand seedRand;

  string configFile;
  string modelsDir;
  string outputDir;
  try {
    TCLAP::CmdLine cmd("Generate training data via self play", ' ', Version::getKataGoVersionForHelp(),true);
    TCLAP::ValueArg<string> configFileArg("","config-file","Config file to use",true,string(),"FILE");
    TCLAP::ValueArg<string> modelsDirArg("","models-dir","Dir to poll and load models from",true,string(),"DIR");
    TCLAP::ValueArg<string> outputDirArg("","output-dir","Dir to output files",true,string(),"DIR");
    cmd.add(configFileArg);
    cmd.add(modelsDirArg);
    cmd.add(outputDirArg);
    cmd.parse(argc,argv);
    configFile = configFileArg.getValue();
    modelsDir = modelsDirArg.getValue();
    outputDir = outputDirArg.getValue();

    auto checkDirNonEmpty = [](const char* flag, const string& s) {
      if(s.length() <= 0)
        throw StringError("Empty directory specified for " + string(flag));
    };
    checkDirNonEmpty("models-dir",modelsDir);
    checkDirNonEmpty("output-dir",outputDir);
  }
  catch (TCLAP::ArgException &e) {
    cerr << "Error: " << e.error() << " for argument " << e.argId() << endl;
    return 1;
  }
  ConfigParser cfg(configFile);

  MakeDir::make(outputDir);
  MakeDir::make(modelsDir);

  Logger logger;
  //Log to random file name to better support starting/stopping as well as multiple parallel runs
  logger.addFile(outputDir + "/log" + DateTime::getCompactDateTimeString() + "-" + Global::uint64ToHexString(seedRand.nextUInt64()) + ".log");
  bool logToStdout = cfg.getBool("logToStdout");
  logger.setLogToStdout(logToStdout);

  logger.write("Self Play Engine starting...");
  logger.write(string("Git revision: ") + Version::getGitRevision());

  //Load runner settings
  const int numGameThreads = cfg.getInt("numGameThreads",1,16384);
  const string searchRandSeedBase = Global::uint64ToHexString(seedRand.nextUInt64());

  //Width and height of the board to use when writing data, typically 19
  const int dataBoardLen = cfg.getInt("dataBoardLen",9,37);
  const int inputsVersion =
    cfg.contains("inputsVersion") ?
    cfg.getInt("inputsVersion",0,10000) :
    NNModelVersion::getInputsVersion(NNModelVersion::defaultModelVersion);
  //Max number of games that we will allow to be queued up and not written out
  const int maxDataQueueSize = cfg.getInt("maxDataQueueSize",1,1000000);
  const int maxRowsPerTrainFile = cfg.getInt("maxRowsPerTrainFile",1,100000000);
  const int maxRowsPerValFile = cfg.getInt("maxRowsPerValFile",1,100000000);
  const double firstFileRandMinProp = cfg.getDouble("firstFileRandMinProp",0.0,1.0);

  const double validationProp = cfg.getDouble("validationProp",0.0,0.5);
  const int64_t maxGamesTotal = cfg.getInt64("numGamesTotal",1,((int64_t)1) << 62);
  const int64_t logGamesEvery = cfg.getInt64("logGamesEvery",1,1000000);

  const bool switchNetsMidGame = cfg.getBool("switchNetsMidGame");
  const SearchParams baseParams = Setup::loadSingleParams(cfg);

  //Initialize object for randomizing game settings and running games
  PlaySettings playSettings = PlaySettings::loadForSelfplay(cfg,dataBoardLen);
  GameRunner* gameRunner = new GameRunner(cfg, searchRandSeedBase, playSettings, logger);
  SelfplayManager* manager = new SelfplayManager(validationProp, maxDataQueueSize, &logger, logGamesEvery);

  Setup::initializeSession(cfg);

  //Done loading!
  //------------------------------------------------------------------------------------
  logger.write("Loaded all config stuff, starting self play");
  if(!logToStdout)
    cout << "Loaded all config stuff, starting self play" << endl;

  if(!std::atomic_is_lock_free(&shouldStop))
    throw StringError("shouldStop is not lock free, signal-quitting mechanism for terminating matches will NOT work!");
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);


  //Returns true if a new net was loaded.
  auto loadLatestNeuralNetIntoManager =
    [inputsVersion,&manager,maxRowsPerTrainFile,maxRowsPerValFile,firstFileRandMinProp,dataBoardLen,
     &modelsDir,&outputDir,&logger,&cfg,numGameThreads](const string* lastNetName) -> bool {

    string modelName;
    string modelFile;
    string modelDir;
    time_t modelTime;
    bool foundModel = LoadModel::findLatestModel(modelsDir, logger, modelName, modelFile, modelDir, modelTime);

    //No new neural nets yet
    if(!foundModel || (lastNetName != NULL && *lastNetName == modelName))
      return false;

    logger.write("Found new neural net " + modelName);

    // * 2 + 16 just in case to have plenty of room
    int maxConcurrentEvals = cfg.getInt("numSearchThreads") * numGameThreads * 2 + 16;
    int defaultMaxBatchSize = -1;

    Rand rand;
    NNEvaluator* nnEval = Setup::initializeNNEvaluator(
      modelName,modelFile,cfg,logger,rand,maxConcurrentEvals,
      NNPos::MAX_BOARD_LEN,NNPos::MAX_BOARD_LEN,defaultMaxBatchSize,
      Setup::SETUP_FOR_OTHER
    );
    logger.write("Loaded latest neural net " + modelName + " from: " + modelFile);

    string modelOutputDir = outputDir + "/" + modelName;
    string sgfOutputDir = modelOutputDir + "/sgfs";
    string tdataOutputDir = modelOutputDir + "/tdata";
    string vdataOutputDir = modelOutputDir + "/vdata";

    //Try repeatedly to make directories, in case the filesystem is unhappy with us as we try to make the same dirs as another process.
    //Wait a random amount of time in between each failure.
    int maxTries = 5;
    for(int i = 0; i<maxTries; i++) {
      bool success = false;
      try {
        MakeDir::make(modelOutputDir);
        MakeDir::make(sgfOutputDir);
        MakeDir::make(tdataOutputDir);
        MakeDir::make(vdataOutputDir);
        success = true;
      }
      catch(const StringError& e) {
        logger.write(string("WARNING, error making directories, trying again shortly: ") + e.what());
        success = false;
      }

      if(success)
        break;
      else {
        if(i == maxTries-1) {
          logger.write("ERROR: Could not make selfplay model directories, is something wrong with the filesystem?");
          //Just give up and wait for the next model.
          return false;
        }
        double sleepTime = 10.0 + rand.nextDouble() * 30.0;
        std::this_thread::sleep_for(std::chrono::duration<double>(sleepTime));
        continue;
      }
    }

    {
      ofstream out(modelOutputDir + "/" + "selfplay-" + Global::uint64ToHexString(rand.nextUInt64()) + ".cfg");
      out << cfg.getContents();
      out.close();
    }

    //Note that this inputsVersion passed here is NOT necessarily the same as the one used in the neural net self play, it
    //simply controls the input feature version for the written data
    TrainingDataWriter* tdataWriter = new TrainingDataWriter(
      tdataOutputDir, inputsVersion, maxRowsPerTrainFile, firstFileRandMinProp, dataBoardLen, dataBoardLen, Global::uint64ToHexString(rand.nextUInt64()));
    TrainingDataWriter* vdataWriter = new TrainingDataWriter(
      vdataOutputDir, inputsVersion, maxRowsPerValFile, firstFileRandMinProp, dataBoardLen, dataBoardLen, Global::uint64ToHexString(rand.nextUInt64()));
    ofstream* sgfOut = sgfOutputDir.length() > 0 ? (new ofstream(sgfOutputDir + "/" + Global::uint64ToHexString(rand.nextUInt64()) + ".sgfs")) : NULL;

    logger.write("Model loading loop thread loaded new neural net " + nnEval->getModelName());
    manager->loadModelAndStartDataWriting(nnEval, tdataWriter, vdataWriter, sgfOut);
    return true;
  };

  //Initialize the initial neural net
  {
    bool success = loadLatestNeuralNetIntoManager(NULL);
    if(!success)
      throw StringError("Either could not load latest neural net or access/write appopriate directories");
  }

  //Check for unused config keys
  cfg.warnUnusedKeys(cerr,&logger);

  //Shared across all game loop threads
  std::atomic<int64_t> numGamesStarted(0);
  ForkData* forkData = new ForkData();
  auto gameLoop = [
    &gameRunner,
    &manager,
    &logger,
    switchNetsMidGame,
    &numGamesStarted,
    &forkData,
    maxGamesTotal,
    &baseParams
  ](int threadIdx) {
    vector<std::atomic<bool>*> stopConditions = {&shouldStop};

    string prevModelName;
    while(true) {
      if(shouldStop.load())
        break;
      NNEvaluator* nnEval = manager->acquireLatest();
      assert(nnEval != NULL);

      if(prevModelName != nnEval->getModelName()) {
        prevModelName = nnEval->getModelName();
        logger.write("Game loop thread " + Global::intToString(threadIdx) + " starting game on new neural net: " + prevModelName);
      }

      //Callback that runGame will call periodically to ask us if we have a new neural net
      std::function<NNEvaluator*()> checkForNewNNEval = [&manager,&nnEval,&prevModelName,&logger,&threadIdx]() -> NNEvaluator* {
        NNEvaluator* newNNEval = manager->acquireLatest();
        assert(newNNEval != NULL);
        if(newNNEval == nnEval) {
          manager->release(newNNEval);
          return NULL;
        }
        manager->release(nnEval);
        nnEval = newNNEval;
        prevModelName = nnEval->getModelName();
        logger.write("Game loop thread " + Global::intToString(threadIdx) + " changing midgame to new neural net: " + prevModelName);
        return nnEval;
      };

      FinishedGameData* gameData = NULL;

      int64_t gameIdx = numGamesStarted.fetch_add(1,std::memory_order_acq_rel);
      manager->countOneGameStarted(nnEval);
      if(gameIdx < maxGamesTotal) {
        MatchPairer::BotSpec botSpecB;
        botSpecB.botIdx = 0;
        botSpecB.botName = nnEval->getModelName();
        botSpecB.nnEval = nnEval;
        botSpecB.baseParams = baseParams;
        MatchPairer::BotSpec botSpecW = botSpecB;
        gameData = gameRunner->runGame(
          gameIdx, botSpecB, botSpecW, forkData, logger,
          stopConditions,
          (switchNetsMidGame ? &checkForNewNNEval : NULL)
        );
      }

      //NULL gamedata will happen when the game is interrupted by stopConditions, which means we should also stop.
      //Or when we run out of total games.
      bool shouldContinue = gameData != NULL;
      //Note that if we've gotten a newNNEval, we're actually pushing the game as data for the new one, rather than the old one!
      if(gameData != NULL)
        manager->enqueueDataToWrite(nnEval,gameData);

      manager->release(nnEval);

      if(!shouldContinue)
        break;
    }

    logger.write("Game loop thread " + Global::intToString(threadIdx) + " terminating");
  };

  //Looping thread for polling for new neural nets and loading them in
  std::mutex modelLoadMutex;
  std::condition_variable modelLoadSleepVar;
  auto modelLoadLoop = [&modelLoadMutex,&modelLoadSleepVar,&logger,&manager,&loadLatestNeuralNetIntoManager]() {
    logger.write("Model loading loop thread starting");

    while(true) {
      if(shouldStop.load())
        break;
      string lastNetName = manager->getLatestModelName();
      bool success = loadLatestNeuralNetIntoManager(&lastNetName);

      if(success) {
        //Mark all older nets draining
        vector<string> modelNames = manager->modelNames();
        assert(modelNames.size() > 0);
        for(size_t i = 0; i < modelNames.size()-1; i++)
          manager->scheduleCleanupModelWhenFree(modelNames[i]);
      }
      if(shouldStop.load())
        break;

      //Sleep for a while and then re-poll
      std::unique_lock<std::mutex> lock(modelLoadMutex);
      modelLoadSleepVar.wait_for(lock, std::chrono::seconds(20), [](){return shouldStop.load();});
    }

    //As part of cleanup, anything remaining, schedule to be freed when possible and prevent new games.
    {
      vector<string> modelNames = manager->modelNames();
      for(size_t i = 0; i < modelNames.size()-1; i++)
        manager->scheduleCleanupModelWhenFree(modelNames[i]);
    }
    logger.write("Model loading loop thread terminating");
  };

  vector<std::thread> threads;
  for(int i = 0; i<numGameThreads; i++) {
    threads.push_back(std::thread(gameLoop,i));
  }
  std::thread modelLoadLoopThread(modelLoadLoop);

  //Wait for all game threads to stop
  for(int i = 0; i<threads.size(); i++)
    threads[i].join();

  //If by now somehow shouldStop is not true, set it to be true since all game threads are toast
  shouldStop.store(true);

  //Wake up the model loading thread rather than waiting for it to wake up on its own, and
  //wait for it to die.
  {
    std::lock_guard<std::mutex> lock(modelLoadMutex);
    modelLoadSleepVar.notify_all();
    modelLoadLoopThread.join();
  }

  //At this point, nothing else except possibly data write loops are running, within the selfplay manager.
  delete manager;

  //Delete and clean up everything else
  NeuralNet::globalCleanup();
  delete forkData;
  delete gameRunner;
  ScoreValue::freeTables();

  if(sigReceived.load())
    logger.write("Exited cleanly after signal");
  logger.write("All cleaned up, quitting");
  return 0;
}
