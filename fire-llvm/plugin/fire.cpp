#include <cassert>
#include <memory>
#include <queue>
#include <stdexcept>
#include <string>
#include <vector>

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/CodeGen/CodeGenAction.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Rewrite/Core/Rewriter.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"

namespace {

static void replace(std::string &Str,
                    std::string const &From,
                    std::string const &To)
{
  auto It { Str.find(From) };
  if (It != std::string::npos)
    Str.replace(It, From.size(), To);
}

class FireError : public std::exception
{
public:
  FireError(std::string const &What, clang::SourceLocation const &Where)
  : What_(What),
    Where_(Where)
  {}

  char const *what() const noexcept override
  { return What_.c_str(); }

  clang::SourceLocation where() const noexcept
  { return Where_; }

private:
  std::string What_;
  clang::SourceLocation Where_;
};

class FireMatchCallback : public clang::ast_matchers::MatchFinder::MatchCallback
{
public:
  FireMatchCallback(clang::ASTContext &Context,
                    clang::FileID *FileID,
                    clang::Rewriter *FileRewriter)
  : Context_(Context),
    FileID_(FileID),
    FileRewriter_(FileRewriter)
  {}

  void run(clang::ast_matchers::MatchFinder::MatchResult const &Result) override
  {
    auto FireCall { Result.Nodes.getNodeAs<clang::CallExpr>("fire") };

    auto FireCallLoc = FireCall->getBeginLoc();

    if (FireCall->getNumArgs() != 1)
      throw FireError("fire::fire_llvm expects exactly one argument", FireCallLoc);

    auto FireArg { llvm::dyn_cast<clang::DeclRefExpr>(FireCall->getArg(0)) };
    if (!FireArg)
      throw FireError("fire::fire_llvm expects a function or class type argument", FireCallLoc);

    clang::FunctionDecl const *FireMain { nullptr };

    for (auto const &FireCallAncestor : getAncestors(*FireCall)) {
      auto MaybeFireMain { FireCallAncestor.get<clang::FunctionDecl>() };

      if (MaybeFireMain && MaybeFireMain->isMain()) {
        FireMain = MaybeFireMain;
        break;
      }
    }

    if (!FireMain)
      throw FireError("fire::fire_llvm must be called inside 'main'", FireCallLoc);

    auto FireMainRange { FireMain->getSourceRange() };

    auto FireFunctionDecl { llvm::dyn_cast<clang::FunctionDecl>(FireArg->getDecl()) };
    assert(FireFunctionDecl); // XXX

    for (auto FireParam : FireFunctionDecl->parameters()) {
      auto FireParamRange { FireParam->getSourceRange() };

      auto FireParamName { FireParam->getName() };

      if (FireParamName.empty())
          throw FireError("Parameter must not be unnamed", FireParamRange.getBegin());

      auto FireParamType { removeLValueRefToConst(FireParam->getType()) };

      auto FireParamDefault {
        FireParam->hasDefaultArg()
          ? printSource(FireParam->getDefaultArgRange())
          : "" };

      std::string NewFireParamType { printType(FireParam->getType()) };
      std::string NewFireParamDefault;

      if (isTypeTemplate(FireParamType, "vector", "std")) {
        NewFireParamDefault = "fire::arg(fire::variadic())";

      } else {
        if (isTypeTemplate(FireParamType, "optional", "std")) {
          replace(NewFireParamType, "std::optional", "fire::optional");

        } else if (!isType(FireParamType, "basic_string") &&
                   !FireParamType->isBooleanType() &&
                   !FireParamType->isIntegerType() &&
                   !FireParamType->isFloatingType()) {

          throw FireError(
            "Parameter must have boolean, integral or floating point type "
            "or be one of std::string, std::vector, std::optional",
            FireParamRange.getBegin());
        }

        auto FireParamDash { FireParamName.size() > 1 ? "--" : "-" };

        if (FireParamDefault.empty()) {
          NewFireParamDefault = llvm::formatv("fire::arg(\"{0}{1}\")",
                                              FireParamDash,
                                              FireParamName);
        } else {
          NewFireParamDefault = llvm::formatv("fire::arg(\"{0}{1}\", {2})",
                                              FireParamDash,
                                              FireParamName,
                                              FireParamDefault);
        }
      }

      std::string NewFireParam { llvm::formatv("{0} {1} = {2}",
                                               NewFireParamType,
                                               FireParamName,
                                               NewFireParamDefault) };

      FileRewriter_->ReplaceText(FireParamRange, NewFireParam);
    }

    std::string NewFireMain { llvm::formatv(
      "FIRE({0})", FireFunctionDecl->getName()) };

    FileRewriter_->ReplaceText(FireMainRange, NewFireMain);

    auto &SourceManager { Context_.getSourceManager() };

    *FileID_ = SourceManager.getFileID(FireCallLoc);
  }

private:
  template<typename T>
  std::vector<clang::DynTypedNode> getAncestors(T const &Node) const
  {
    std::vector<clang::DynTypedNode> Ancestors;

    std::queue<clang::DynTypedNode> ParentQueue;

    for (auto const &Parent : Context_.getParents(Node))
      ParentQueue.push(Parent);

    while (!ParentQueue.empty()) {
      std::size_t NumParents { ParentQueue.size() };

      for (std::size_t i = 0; i < NumParents; ++i) {
        auto Parent { ParentQueue.front() };
        ParentQueue.pop();

        Ancestors.push_back(Parent);

        for (auto const &GrandParent : Context_.getParents(Parent))
          ParentQueue.push(GrandParent);
      }
    }

    return Ancestors;
  }

  static clang::QualType removeLValueRefToConst(clang::QualType Type)
  {
    if (Type->isLValueReferenceType()) {
      auto ReferencedType { Type.getNonReferenceType() };

      if (ReferencedType.isConstQualified())
        return ReferencedType;
    }

    return Type;
  }

  static bool isType(clang::QualType Type,
                     std::string const &Name,
                     std::string const &Namespace = "")
  {
    auto R { Type->getAs<clang::RecordType>() };
    if (!R)
      return false;

    auto RD { R->getDecl() };
    if (!RD)
      return false;

    if (!Namespace.empty() && !isInNamespace(RD, Namespace))
      return false;

    return RD->getName() == Name;
  }

  static bool isTypeTemplate(clang::QualType Type,
                             std::string const &Name,
                             std::string const &Namespace = "")
  {
    auto TS { Type->getAs<clang::TemplateSpecializationType>() };
    if (!TS)
      return false;

    auto TM { TS->getTemplateName() };
    auto TD { TM.getAsTemplateDecl() };
    if (!TD)
      return false;

    if (!Namespace.empty() && !isInNamespace(TD, Namespace))
      return false;

    return TD->getName() == Name;
  }

  template<typename T>
  static bool isInNamespace(T const *Decl, std::string const &Namespace)
  {
     auto ND { llvm::dyn_cast<clang::NamespaceDecl>(Decl->getDeclContext()) };
     if (!ND)
       return false;

     auto II { ND->getIdentifier() };
     if (!II || II->getName() != Namespace)
       return false;

     return llvm::isa<clang::TranslationUnitDecl>(ND->getDeclContext());
  }

  std::string printType(clang::QualType const &Type) const
  {
    auto &LangOpts { Context_.getLangOpts() };

    clang::PrintingPolicy PP { LangOpts };

    return Type.getAsString(PP);
  }

  std::string printSource(clang::SourceRange const &Range) const
  {
    auto &SourceManager { Context_.getSourceManager() };

    auto Begin { SourceManager.getCharacterData(Range.getBegin()) };
    auto End { SourceManager.getCharacterData(Range.getEnd()) };

    return std::string(Begin, End - Begin + 1);
  }

  clang::ASTContext &Context_;
  clang::FileID *FileID_;
  clang::Rewriter *FileRewriter_;
};

class FireConsumer : public clang::ASTConsumer
{
public:
  FireConsumer(clang::FileID *FileID,
               clang::Rewriter *FileRewriter,
               bool *FileRewriteError)
  : FileID_(FileID),
    FileRewriter_(FileRewriter),
    FileRewriteError_(FileRewriteError)
  {}

  void HandleTranslationUnit(clang::ASTContext &Context) override
  {
    using namespace clang::ast_matchers;

    // create match expression
    auto FireMatchExpression {
      callExpr(
        callee(
          functionDecl(
            allOf(
              hasName("fire_llvm"),
              hasParent(
                functionTemplateDecl(
                  hasParent(
                    namespaceDecl(
                      hasName("fire")))))))))
    };

    // create match callback
    FireMatchCallback MatchCallback(Context, FileID_, FileRewriter_);

    // create and run match finder
    clang::ast_matchers::MatchFinder MatchFinder;
    MatchFinder.addMatcher(FireMatchExpression.bind("fire"), &MatchCallback);

    try {
      MatchFinder.matchAST(Context);

      *FileRewriteError_ = false;

    } catch (FireError const &e) {
      auto &Diags { Context.getDiagnostics() };

      unsigned ID { Diags.getDiagnosticIDs()->getCustomDiagID(
                      clang::DiagnosticIDs::Error, e.what()) };

      Diags.Report(e.where(), ID);

      *FileRewriteError_ = true;
    }
  }

private:
  clang::FileID *FileID_;
  clang::Rewriter *FileRewriter_;
  bool *FileRewriteError_;
};

class FireAction : public clang::PluginASTAction
{
protected:
  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
    clang::CompilerInstance &CI,
    llvm::StringRef FileName) override
  {
    auto &SourceManager { CI.getSourceManager() };
    auto &LangOpts { CI.getLangOpts() };

    CI_ = &CI;

    FileName_ = FileName.str();

    FileRewriter_.setSourceMgr(SourceManager, LangOpts);

    return std::make_unique<FireConsumer>(
        &FileID_, &FileRewriter_, &FileRewriteError_);
  }

  bool ParseArgs(clang::CompilerInstance const &,
                 std::vector<std::string> const &) override
  {
    return true;
  }

  clang::PluginASTAction::ActionType getActionType() override
  {
    return clang::PluginASTAction::ReplaceAction;
  }

  void EndSourceFileAction() override
  {
    if (FileRewriteError_)
      return;

    auto &CodeGenOpts { CI_->getCodeGenOpts() };
    auto &Target { CI_->getTarget() };
    auto &Diagnostics { CI_->getDiagnostics() };

    // retrieve rewrite buffer
    auto FileRewriteBuffer { FileRewriter_.getRewriteBufferFor(FileID_) };

    std::string FileContent {
        FileRewriteBuffer->begin(), FileRewriteBuffer->end() };

    auto FileMemoryBuffer { llvm::MemoryBuffer::getMemBufferCopy(FileContent) };

    // create new compiler instance
    auto CInvNew { std::make_shared<clang::CompilerInvocation>() };

    assert(clang::CompilerInvocation::CreateFromArgs(
      *CInvNew, CodeGenOpts.CommandLineArgs, Diagnostics));

    clang::CompilerInstance CINew;
    CINew.setInvocation(CInvNew);
    CINew.setTarget(&Target);
    CINew.createDiagnostics();

    // create "virtual" input file
    auto &PreprocessorOpts { CINew.getPreprocessorOpts() };

    PreprocessorOpts.addRemappedFile(FileName_, FileMemoryBuffer.get());

    // generate code
    clang::EmitObjAction EmitObj;
    CINew.ExecuteAction(EmitObj);

    // clean up rewrite buffer
    FileMemoryBuffer.release();
  }

private:
  clang::CompilerInstance *CI_;

  std::string FileName_;
  clang::FileID FileID_;
  clang::Rewriter FileRewriter_;
  bool FileRewriteError_ = false;
};

} // end namespace

static clang::FrontendPluginRegistry::Add<FireAction>
X("fire", "create CLI from functions or classes");
