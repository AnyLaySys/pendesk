class App extends $falcon.App {}

App.meta = {
  pages: { index: "index" },
  options: { style: { lessPaths: [], themes: [] } },
  meta: {}
};
App.meta.name = "pendesk";
App.meta.version = "0.6.0";
App.meta.isSingleJsBundle = false;
$falcon.__AppClazz = App;
$falcon.__loadModuleDefault = async function (fileName) {
  return (await import("./" + fileName + ".js")).default;
};
$falcon.__KEYFRAMES = [];