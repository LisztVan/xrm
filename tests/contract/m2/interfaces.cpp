// XRM: Checks public interfaces at compile time.

#include "contract/dtos.hpp"
#include "contract/methods.hpp"
#include "infra/http.hpp"
#include "infra/store.hpp"
#include "subscription/parser.hpp"
#include "subscription/reconcile.hpp"
#include "subscription/subscription_service.hpp"

#include <type_traits>
#include <utility>

static_assert(std::is_default_constructible_v<xrm::Subscription>);
static_assert(std::is_default_constructible_v<xrm::ParseReport>);
static_assert(std::is_default_constructible_v<xrm::RefreshReport>);
static_assert(std::is_default_constructible_v<xrm::SourceCommit>);
static_assert(
    std::is_same_v<decltype(xrm::parse_subscription(std::string_view{})),
                   xrm::Result<xrm::ParseReport>>);
static_assert(
    std::is_same_v<decltype(xrm::reconcile(
                       std::declval<const xrm::ReconcileInput &>(),
                       std::declval<const std::function<xrm::Id()> &>())),
                   xrm::Result<xrm::ReconcileResult>>);
static_assert(
    std::is_same_v<decltype(std::declval<xrm::HttpClient &>().fetch(
                       std::declval<xrm::HttpRequest>(),
                       std::declval<xrm::Completion<xrm::HttpResponse>>())),
                   xrm::Result<xrm::Id>>);
static_assert(
    std::is_same_v<decltype(std::declval<xrm::Store &>().commit_source(
                       std::declval<const xrm::Id &>(),
                       std::declval<const xrm::SourceCommit &>())),
                   xrm::Result<void>>);
